#include "nodes_monitor.h"

#include <experimental/optional>

#include <folly/io/async/EventBaseManager.h>


using std::experimental::optional;
using std::experimental::nullopt;

const std::string kNodesKey = "nodes";

NodesMonitor::NodesMonitor(const std::string& host, uint port, std::shared_ptr<EtcdClient> etcd_client) :
        etcd_client_(std::move(etcd_client)),
        evb_(etcd_client_->get_event_base())
{
    assert(etcd_client_);
    const std::string port_str = std::to_string(port);
    etcd_key_ = kNodesKey + "/";
    etcd_key_.append(host);
    etcd_key_.append("_");
    etcd_key_.append(port_str);
    host_port_ = host + ':';
    host_port_.append(port_str);
    UpdateAll();
}

NodesMonitor::~NodesMonitor() {}

static optional<std::pair<folly::SocketAddress, bool>>
EtcdNodeToAddr(EtcdNode& node, const std::string& self_host_port) {
    if (node.is_dir) {
        LOG(ERROR) << "Invalid type 'dir' of etcd node " << node.name << "!";
        return nullopt;
    }
    const std::string& value = node.value;
    bool is_self_addr = (value == self_host_port);
    auto delimiter_pos = value.find(':');
    if (delimiter_pos == value.npos) {
        LOG(ERROR) << "Invalid host:port value: " << value;
        return nullopt;
    }

    try {
        std::uint16_t port = std::stoi(value.substr(delimiter_pos + 1));
        return std::make_pair(folly::SocketAddress(value.substr(0, delimiter_pos), port, true), is_self_addr);
    } catch (const std::exception& e) {
        LOG(ERROR) << "Failed to resolve hostname \"" << node.value << "\": " << e.what();
    }
    return nullopt;
}

static auto addr_vec_comp = [](const NodesMonitor::addr_entry_t& a, const NodesMonitor::addr_entry_t& b) {
    return a.first < b.first;
};

void NodesMonitor::UpdateAll() {
    auto task = std::make_shared<EtcdClient::GetTask>([this](EtcdResponse response){
        update_id_ = response.etcd_id + 1;
        EtcdNode& etcd_node = *response.node;
        auto addr_vec = std::make_shared<addr_vec_t>();
        addr_vec ->reserve(etcd_node.subnodes.size());
        for (auto& subnode : etcd_node.subnodes) {
            assert(subnode);
            auto addr = EtcdNodeToAddr(*subnode, host_port_);
            if (addr) {
                addr_vec ->push_back(std::move(*addr));
            }
        }
        std::sort(addr_vec->begin(), addr_vec->end(), addr_vec_comp);
        std::atomic_store(&addr_vec_, std::move(addr_vec ));
        Watch();
    }, [this](EtcdError err){
        if (err == EtcdError::pending_shutdown) {
            return;
        }
        LOG(ERROR) << err;

        UpdateAll();
    }, false);
    etcd_client_->Get(std::move(task), kNodesKey);
}

void NodesMonitor::Watch() {
    auto task = std::make_shared<EtcdClient::WatchTask>([this](std::shared_ptr<EtcdUpdate> update) {
        assert(update->new_node);
        EtcdNode& etcd_node = *update->new_node;
        update_id_ = etcd_node.modified_id + 1;
        if (update->type == EtcdUpdate::Type::set) {
            auto new_addr_entry = EtcdNodeToAddr(etcd_node, host_port_);
            if (new_addr_entry) {
                auto new_addr_vec = std::make_shared<addr_vec_t>(*addr_vec_);
                new_addr_vec->push_back(std::move(*new_addr_entry));
                std::sort(new_addr_vec->begin(), new_addr_vec->end(), addr_vec_comp);
                std::atomic_store(&addr_vec_, std::move(new_addr_vec));
            }
        } else if (update->type == EtcdUpdate::Type::remove) {
            auto new_addr_vec = std::make_shared<addr_vec_t>();
            if (!addr_vec_->empty()) {
                new_addr_vec->reserve(addr_vec_->size() - 1);
            }
            for (auto addr_entry_itr = addr_vec_->begin(); addr_entry_itr != addr_vec_->end(); ++addr_entry_itr) {
                const folly::SocketAddress& addr = addr_entry_itr->first;
                if (addr.getAddressStr() + ':' + std::to_string(addr.getPort()) == etcd_node.value) {
                    new_addr_vec->insert(new_addr_vec->end(), ++addr_entry_itr, addr_vec_->end());
                    break;
                }
                new_addr_vec->push_back(*addr_entry_itr);
            }
            std::atomic_store(&addr_vec_, std::move(new_addr_vec));
        }
        Watch();
    }, [this](EtcdError err){
        if (err == EtcdError::pending_shutdown) {
            return;
        }
        if (err == EtcdError::wait_id_outdated) {
            UpdateAll();
            return;
        }
        if (err == EtcdError::connection_timeout) {
            Watch();
        }
        LOG(ERROR) << err;
        evb_.runAfterDelay([this]{ Watch(); }, 500);
    }, false);
    etcd_client_->Watch(std::move(task), kNodesKey, update_id_);
}

void NodesMonitor::Register() {
    if (registered_) {
        return;
    }
    bool expected = false;
    if (!pending_registration_.compare_exchange_strong(expected, true)) {
        return;
    }
    auto task = std::make_shared<EtcdClient::UpdateTask>([this]{
        registered_ = true;
        pending_registration_ = false;
        evb_.runAfterDelay([this]{ UpdateRegistration(); }, 5000);
    }, [this](EtcdError err){
        if (err == EtcdError::pending_shutdown) {
            return;
        }
        pending_registration_ = false;
        Register();
    }, false);

    etcd_client_->Set(std::move(task), etcd_key_, host_port_, 10, false);
}

void NodesMonitor::UpdateRegistration() {
    if (!registered_) {
        return;
    }
    auto task = std::make_shared<EtcdClient::UpdateTask>([this]{
        if (registered_) {
            // Update only if still registered
            evb_.runAfterDelay([this]{ UpdateRegistration(); }, 5000);
        }
    }, [this](EtcdError err){
        if (err == EtcdError::pending_shutdown) {
            return;
        }
        evb_.runAfterDelay([this]{ UpdateRegistration(); }, 500);
    }, false);
    etcd_client_->Set(std::move(task), etcd_key_, host_port_, 10, true);
}

void NodesMonitor::Unregister() {
    if (!registered_) {
        return;
    }
    registered_ = false;
    etcd_client_->Delete(std::make_shared<EtcdClient::UpdateTask>(), etcd_key_);
}

std::shared_ptr<const NodesMonitor::addr_vec_t> NodesMonitor::GetActiveNodes() const {
    return std::atomic_load(&addr_vec_);
}