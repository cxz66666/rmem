#pragma once
#include <hdr/hdr_histogram.h>
#include "atomic_queue/atomic_queue.h"
#include "../img_transcode_commons.h"

class ClientContext : public BasicContext
{
public:
    ClientContext(size_t cid, size_t sid, size_t rid) : client_id_(cid), server_sender_id_(sid), server_receiver_id_(rid)
    {
        forward_spsc_queue = new atomic_queue::AtomicQueueB2<erpc::MsgBuffer, std::allocator<erpc::MsgBuffer>, true, false, true>(FLAGS_concurrency);
        backward_spsc_queue = new atomic_queue::AtomicQueueB2<erpc::MsgBuffer, std::allocator<erpc::MsgBuffer>, true, false, false>(FLAGS_concurrency);
    }
    ~ClientContext()
    {
        delete forward_spsc_queue;
        delete backward_spsc_queue;
    }
    erpc::MsgBuffer req_forward_msgbuf[kAppMaxConcurrency];
    erpc::MsgBuffer req_backward_msgbuf[kAppMaxConcurrency];

    erpc::MsgBuffer resp_forward_msgbuf[kAppMaxConcurrency];
    erpc::MsgBuffer resp_backward_msgbuf[kAppMaxConcurrency];

    size_t client_id_;
    size_t server_sender_id_;
    size_t server_receiver_id_;

    atomic_queue::AtomicQueueB2<erpc::MsgBuffer, std::allocator<erpc::MsgBuffer>, true, false, true> *forward_spsc_queue;
    atomic_queue::AtomicQueueB2<erpc::MsgBuffer, std::allocator<erpc::MsgBuffer>, true, false, false> *backward_spsc_queue;
};

class ServerContext : public BasicContext
{
public:
    ServerContext(size_t sid) : server_id_(sid), stat_req_ping_tot(0), stat_req_ping_resp_tot(0), stat_req_tc_tot(0), stat_req_tc_req_tot(0), stat_req_err_tot(0)
    {
    }
    ~ServerContext()
    {
    }
    size_t server_id_;
    size_t stat_req_ping_tot;
    size_t stat_req_ping_resp_tot;
    size_t stat_req_tc_tot;
    size_t stat_req_tc_req_tot;
    size_t stat_req_err_tot;

    void reset_stat()
    {
        stat_req_ping_tot = 0;
        stat_req_ping_resp_tot = 0;
        stat_req_tc_tot = 0;
        stat_req_tc_req_tot = 0;
        stat_req_err_tot = 0;
    }

    atomic_queue::AtomicQueueB2<erpc::MsgBuffer, std::allocator<erpc::MsgBuffer>, true, false, true> *forward_spsc_queue;
    atomic_queue::AtomicQueueB2<erpc::MsgBuffer, std::allocator<erpc::MsgBuffer>, true, false, false> *backward_spsc_queue;
};

class AppContext
{
public:
    AppContext()
    {
        int ret = hdr_init(1, 1000 * 1000 * 10, 3,
                           &latency_hist_);
        rmem::rt_assert(ret == 0, "hdr_init failed");
        for (size_t i = 0; i < FLAGS_client_num; i++)
        {
            client_contexts_.push_back(new ClientContext(i, i % FLAGS_server_forward_num, i % FLAGS_server_backward_num));
        }
        for (size_t i = 0; i < FLAGS_server_num; i++)
        {
            server_contexts_.push_back(new ServerContext(i));
            // init queue
            server_contexts_[i]->forward_spsc_queue = client_contexts_[i]->forward_spsc_queue;
            server_contexts_[i]->backward_spsc_queue = client_contexts_[i]->backward_spsc_queue;
        }
    }
    ~AppContext()
    {
        hdr_close(latency_hist_);

        for (auto &ctx : client_contexts_)
        {
            delete ctx;
        }
        for (auto &ctx : server_contexts_)
        {
            delete ctx;
        }
    }
    bool write_latency_and_reset(std::string filename)
    {

        FILE *fp = fopen(filename.c_str(), "w");
        if (fp == nullptr)
        {
            return false;
        }
        hdr_percentiles_print(latency_hist_, fp, 5, 10, CLASSIC);
        fclose(fp);
        hdr_reset(latency_hist_);
        return true;
    }

    uint32_t req_number_;

    std::vector<ClientContext *> client_contexts_;
    std::vector<ServerContext *> server_contexts_;

    hdr_histogram *latency_hist_;
};
