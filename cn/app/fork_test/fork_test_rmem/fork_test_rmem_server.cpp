#include <thread>
#include "numautil.h"
#include "spinlock_mutex.h"
#include "fork_test_rmem.h"

size_t get_bind_core(size_t numa)
{
    static size_t numa0_core = 0;
    static size_t numa1_core = 0;
    static spinlock_mutex lock;
    size_t res;
    lock.lock();
    rmem::rt_assert(numa == 0 || numa == 1);
    if (numa == 0)
    {
        rmem::rt_assert(numa0_core <= rmem::num_lcores_per_numa_node());
        res = numa0_core++;
    }
    else
    {
        rmem::rt_assert(numa1_core <= rmem::num_lcores_per_numa_node());
        res = numa1_core++;
    }
    lock.unlock();
    return res;
}

void ping_handler(erpc::ReqHandle *req_handle, void *_context)
{
    auto *ctx = static_cast<ServerContext *>(_context);
    ctx->stat_req_ping_tot++;
    auto *req_msgbuf = req_handle->get_req_msgbuf();
    rmem::rt_assert(req_msgbuf->get_data_size() == sizeof(PingReq), "data size not match");

    auto *req = reinterpret_cast<PingReq *>(req_msgbuf->buf_);

    ctx->rmem_ = new rmem::Rmem(0);
    printf("hosts %s, thread_id %u, session_id %u\n", req->ping_param.hosts, req->ping_param.rmem_thread_id_, req->ping_param.rmem_session_id_);
    std::string hosts(req->ping_param.hosts);
    if (unlikely(ctx->rmem_->connect_session(hosts, req->ping_param.rmem_thread_id_)) != 0)
    {
        printf("connect error\n");
        exit(-1);
    }
    ctx->rmem_thread_id_ = req->ping_param.rmem_thread_id_;
    ctx->rmem_session_id_ = req->ping_param.rmem_session_id_;
    new (req_handle->pre_resp_msgbuf_.buf_) PingResp(req->req.type, req->req.req_number, 0, req->timestamp);
    ctx->rpc_->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, sizeof(PingResp));

    ctx->rpc_->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
}

void transcode_handler(erpc::ReqHandle *req_handle, void *_context)
{
    auto *ctx = static_cast<ServerContext *>(_context);
    ctx->stat_req_tc_tot++;
    auto *req_msgbuf = req_handle->get_req_msgbuf();

    auto *req = reinterpret_cast<RmemReq *>(req_msgbuf->buf_);
    rmem::rt_assert(req_msgbuf->get_data_size() == sizeof(RmemReq), "data size not match");

    // printf("receive new transcode resp, length is %zu, req number is %u\n", req->extra.length, req->req.req_number);

    unsigned long addr = ctx->rmem_->rmem_join(req->extra.addr, ctx->rmem_thread_id_, ctx->rmem_session_id_);

    for (size_t i = 0; i < FLAGS_write_num; i++)
    {
        ctx->rmem_->rmem_write_async(ctx->write_buf, addr + i * PAGE_SIZE, FLAGS_write_page_size);
    }

    size_t res = 0;
    int results[10];
    while (res < FLAGS_write_num)
    {
        res += ctx->rmem_->rmem_poll(results, FLAGS_write_num);
    }

    new (req_handle->pre_resp_msgbuf_.buf_) RmemResp(req->req.type, req->req.req_number, 0);

    ctx->rpc_->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
}

void server_thread_func(size_t thread_id, ServerContext *ctx, erpc::Nexus *nexus)
{
    ctx->server_id_ = thread_id;
    std::vector<size_t> port_vec = flags_get_numa_ports(0);
    uint8_t phy_port = port_vec.at(thread_id % port_vec.size());
    erpc::Rpc<erpc::CTransport> rpc(nexus, static_cast<void *>(ctx),
                                    static_cast<uint8_t>(thread_id),
                                    basic_sm_handler_server, phy_port);
    rpc.retry_connect_on_invalid_rpc_id_ = true;
    ctx->rpc_ = &rpc;
    while (true)
    {
        ctx->reset_stat();
        erpc::ChronoTimer start;
        start.reset();
        rpc.run_event_loop(kAppEvLoopMs);
        const double seconds = start.get_sec();
        printf("thread %zu: ping_req : %.2f, tc : %.2f \n", thread_id,
               ctx->stat_req_ping_tot / seconds, ctx->stat_req_tc_tot / seconds);

        ctx->rpc_->reset_dpath_stats();
        // more handler
        if (ctrl_c_pressed == 1)
        {
            break;
        }
    }
}

void leader_thread_func()
{
    erpc::Nexus nexus(rmem::get_uri_for_process(FLAGS_server_index),
                      FLAGS_numa_server_node, 0);

    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_PING), ping_handler);

    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_TRANSCODE), transcode_handler);

    std::vector<std::thread> servers(FLAGS_server_num);

    auto *context = new AppContext_Server();

    servers[0] = std::thread(server_thread_func, 0, context->server_contexts_[0], &nexus);
    sleep(2);
    rmem::bind_to_core(servers[0], FLAGS_numa_server_node, get_bind_core(FLAGS_numa_server_node) + FLAGS_bind_core_offset);

    for (size_t i = 1; i < FLAGS_server_num; i++)
    {
        servers[i] = std::thread(server_thread_func, i, context->server_contexts_[i], &nexus);

        rmem::bind_to_core(servers[i], FLAGS_numa_server_node, get_bind_core(FLAGS_numa_server_node) + FLAGS_bind_core_offset);
    }
    sleep(2);

    if (FLAGS_timeout_second != UINT64_MAX)
    {
        sleep(FLAGS_timeout_second);
        ctrl_c_pressed = true;
    }

    for (size_t i = 0; i < FLAGS_server_num; i++)
    {
        servers[i].join();
    }
}

int main(int argc, char **argv)
{
    signal(SIGINT, ctrl_c_handler);
    signal(SIGTERM, ctrl_c_handler);
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    check_common_gflags();
    rmem::rmem_init(rmem::get_uri_for_process(FLAGS_rmem_self_index), FLAGS_numa_client_node);

    std::thread leader_thread(leader_thread_func);
    leader_thread.join();
}
