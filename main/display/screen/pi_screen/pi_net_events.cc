#include "pi_net_events.h"

#include <mutex>
#include <utility>
#include <vector>

namespace {

struct Entry {
    int id;
    pi_net_events::Listener cb;
};

std::mutex s_mu;
bool s_subscribed = false;
int s_next_id = 1;
std::vector<Entry> s_listeners;

// 网络栈任务线程上下文。持锁只做列表拷贝，回调在锁外执行：监听者回调里
// 再调 Add/RemoveListener 不会死锁；Remove 后仍可能收到一次已在途的回调
// （与 mhal 层"回调在别的线程"这一既有事实同级，监听者本来就要自把稳）。
void Fanout(mhal::network::Event e, const std::string& data) {
    std::vector<pi_net_events::Listener> cbs;
    {
        std::lock_guard<std::mutex> lk(s_mu);
        cbs.reserve(s_listeners.size());
        for (const auto& l : s_listeners)
            cbs.push_back(l.cb);
    }
    for (auto& cb : cbs) {
        if (cb)
            cb(e, data);
    }
}

}  // namespace

namespace pi_net_events {

void Init() {
    {
        std::lock_guard<std::mutex> lk(s_mu);
        if (s_subscribed)
            return;
        s_subscribed = true;
    }
    mhal::network::OnEvent(Fanout);
}

int AddListener(Listener cb) {
    Init();
    std::lock_guard<std::mutex> lk(s_mu);
    int id = s_next_id++;
    s_listeners.push_back({id, std::move(cb)});
    return id;
}

void RemoveListener(int id) {
    std::lock_guard<std::mutex> lk(s_mu);
    for (auto it = s_listeners.begin(); it != s_listeners.end(); ++it) {
        if (it->id == id) {
            s_listeners.erase(it);
            break;
        }
    }
}

}  // namespace pi_net_events
