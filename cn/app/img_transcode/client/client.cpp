#include <thread>
#include "numautil.h"
#include "spinlock_mutex.h"
#include "client.h"

size_t get_bind_core(size_t numa)
{
    static size_t numa0_core;
    static size_t numa1_core;
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

void connect_sessions(ClientContext *c)
{
    std::string remote_uri = rmem::get_uri_for_process(FLAGS_server_forward_index);
    int session_num = c->rpc_->create_session(remote_uri, c->server_sender_id_);
    rmem::rt_assert(session_num >= 0, "Failed to create session");
    c->session_num_vec_.push_back(session_num);
    while (c->num_sm_resps_ != 1)
    {
        c->rpc_->run_event_loop(kAppEvLoopMs);
        if (unlikely(ctrl_c_pressed == 1))
        {
            printf("Ctrl-C pressed. Exiting\n");
            return;
        }
    }
}

void ping_resp_handler(erpc::ReqHandle *req_handle, void *_context)
{
    ServerContext *ctx = static_cast<ServerContext *>(_context);
    ctx->stat_req_ping_resp_tot++;
    auto *req_msgbuf = req_handle->get_req_msgbuf();
    rmem::rt_assert(req_msgbuf->get_data_size() == sizeof(PingReq), "data size not match");

    auto *req = reinterpret_cast<PingReq *>(req_msgbuf->buf_);

    new (req_handle->pre_resp_msgbuf_.buf_) PingResp(req->req.type, req->req.req_number, req->timestamp);
    ctx->rpc_->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, sizeof(PingResp));
    ctx->rpc_->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);

    ctx->spsc_queue->push(RESP_MSG{req->req.req_number, 0});
}

void transcode_resp_handler(erpc::ReqHandle *req_handle, void *_context)
{
    ServerContext *ctx = static_cast<ServerContext *>(_context);
    ctx->stat_req_tc_req_tot++;
    auto *req_msgbuf = req_handle->get_req_msgbuf();

    auto *req = reinterpret_cast<TranscodeReq *>(req_msgbuf->buf_);
    rmem::rt_assert(req_msgbuf->get_data_size() == sizeof(TranscodeReq) + req->extra.length, "data size not match");

    printf("receive new transcode resp, length is %zu, req number is %u\n", req->extra.length, req->req.req_number);

    new (req_handle->pre_resp_msgbuf_.buf_) TranscodeResp(req->req.type, req->req.req_number, req->extra.length);

    ctx->rpc_->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, sizeof(TranscodeResp));

    ctx->rpc_->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);

    ctx->spsc_queue->push(RESP_MSG{req->req.req_number, 0});
}

void callback_ping(void *_context, void *_tag)
{
    _unused(_tag);
    ClientContext *ctx = static_cast<ClientContext *>(_context);

    PingResp *resp = reinterpret_cast<PingResp *>(ctx->ping_resp_msgbuf.buf_);

    // 如果返回值不为0，则认为后续不会有响应，直接将请求号和错误码放入队列
    // 如果返回值为0，则认为后续将有响应，不care
    if (resp->resp.status != 0)
    {
        ctx->resp_spsc_queue->push(RESP_MSG{resp->resp.req_number, resp->resp.status});
    }
}

void handler_ping(ClientContext *ctx, REQ_MSG req_msg)
{

    new (ctx->ping_msgbuf.buf_) PingReq(RPC_TYPE::RPC_PING, req_msg.req_id, SIZE_MAX);
    ctx->rpc_->enqueue_request(ctx->session_num_vec_[0], static_cast<uint8_t>(RPC_TYPE::RPC_PING),
                               &ctx->ping_msgbuf, &ctx->ping_resp_msgbuf,
                               callback_ping, nullptr);
}

void callback_tc(void *_context, void *_tag)
{
    uint32_t req_id = reinterpret_cast<uint32_t>(_tag);
    ClientContext *ctx = static_cast<ClientContext *>(_context);

    TranscodeResp *resp = reinterpret_cast<TranscodeResp *>(ctx->req_msgbuf[req_id % kAppMaxConcurrency].buf_);

    if (resp->resp.status != 0)
    {
        ctx->resp_spsc_queue->push(RESP_MSG{resp->resp.req_number, resp->resp.status});
    }
}
void handler_tc(ClientContext *ctx, REQ_MSG req_msg)
{
    erpc::MsgBuffer &req_msgbuf = ctx->req_msgbuf[req_msg.req_id % kAppMaxConcurrency];
    erpc::MsgBuffer &resp_msgbuf = ctx->resp_msgbuf[req_msg.req_id % kAppMaxConcurrency];
    // TODO don't know length, a hack method
    new (req_msgbuf.buf_) TranscodeReq(RPC_TYPE::RPC_TRANSCODE, req_msg.req_id, req_msgbuf.get_data_size() - sizeof(TranscodeReq));

    ctx->rpc_->enqueue_request(ctx->session_num_vec_[0], static_cast<uint8_t>(RPC_TYPE::RPC_TRANSCODE),
                               &req_msgbuf, &resp_msgbuf,
                               callback_tc, reinterpret_cast<void *>(req_msg.req_id));
}

void client_thread_func(size_t thread_id, ClientContext *ctx, erpc::Nexus *nexus)
{
    ctx->client_id_ = thread_id;
    std::vector<size_t> port_vec = flags_get_numa_ports(0);
    uint8_t phy_port = port_vec.at(thread_id % port_vec.size());
    erpc::Rpc<erpc::CTransport> rpc(nexus, static_cast<void *>(ctx),
                                    static_cast<uint8_t>(thread_id),
                                    basic_sm_handler, phy_port);
    rpc.retry_connect_on_invalid_rpc_id_ = true;
    ctx->rpc_ = &rpc;
    for (size_t i = 0; i < FLAGS_concurrency; i++)
    {
        // TODO
        ctx->req_msgbuf[i] = rpc.alloc_msg_buffer_or_die(0);
        ctx->resp_msgbuf[i] = rpc.alloc_msg_buffer_or_die(0);
    }
    ctx->ping_msgbuf = rpc.alloc_msg_buffer_or_die(sizeof(PingReq));
    ctx->ping_resp_msgbuf = rpc.alloc_msg_buffer_or_die(sizeof(PingResp));

    connect_sessions(ctx);

    using FUNC_HANDLER = std::function<void(ClientContext *, REQ_MSG)>;
    FUNC_HANDLER handlers[] = {handler_ping, handler_tc, nullptr};

    while (1)
    {
        unsigned size = ctx->spsc_queue->was_size();
        for (unsigned i = 0; i < size; i++)
        {
            REQ_MSG req_msg = ctx->spsc_queue->pop();
            handlers[static_cast<uint8_t>(req_msg.req_type)](ctx, req_msg);
        }
        ctx->rpc_->run_event_loop_once();
    }
}

void server_thread_func(size_t thread_id, ServerContext *ctx, erpc::Nexus *nexus)
{
    ctx->server_id_ = thread_id;
    std::vector<size_t> port_vec = flags_get_numa_ports(0);
    uint8_t phy_port = port_vec.at(thread_id % port_vec.size());
    erpc::Rpc<erpc::CTransport> rpc(nexus, static_cast<void *>(ctx),
                                    static_cast<uint8_t>(thread_id),
                                    basic_sm_handler, phy_port);
    rpc.retry_connect_on_invalid_rpc_id_ = true;
    ctx->rpc_ = &rpc;
    while (true)
    {
        ctx->reset_stat();
        erpc::ChronoTimer start;
        start.reset();
        rpc.run_event_loop(kAppEvLoopMs);
        const double seconds = start.get_sec();
        printf("thread %zu: %.2f M/s. ping_req : %.3lf, tc : %.3f, tc_req : %.3f \n", thread_id,
               ctx->stat_req_ping_tot / (seconds * Mi(1)), ctx->stat_req_tc_tot / (seconds * Mi(1)), ctx->stat_req_tc_req_tot / (seconds * Mi(1)));

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

    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_PING_RESP), ping_resp_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_TRANSCODE_RESP), transcode_resp_handler);

    std::vector<std::thread> clients(FLAGS_client_num);
    std::vector<std::thread> servers(FLAGS_server_num);

    AppContext *context = new AppContext();

    clients[0] = std::thread(client_thread_func, 0, context->client_contexts_[0]);
    sleep(2);
    rmem::bind_to_core(clients[0], FLAGS_numa_client_node, get_bind_core(FLAGS_numa_client_node));

    for (size_t i = 1; i < FLAGS_client_num; i++)
    {
        clients[i] = std::thread(client_thread_func, i, context->client_contexts_[i]);
        rmem::bind_to_core(clients[i], FLAGS_numa_client_node, get_bind_core(FLAGS_numa_client_node));
    }

    for (size_t i = 0; i < FLAGS_server_num; i++)
    {
        servers[i] = std::thread(server_thread_func, i, context->server_contexts_[i]);
        rmem::bind_to_core(servers[i], FLAGS_numa_server_node, get_bind_core(FLAGS_numa_server_node));
    }
    sleep(10);

    for (size_t i = 0; i < FLAGS_client_num; i++)
    {
        context->client_contexts_[i]->spsc_queue->push(REQ_MSG{0, RPC_TYPE::RPC_PING});
    }
    for (size_t i = 0; i < FLAGS_server_num; i++)
    {
        // connect success
        RESP_MSG msg = context->server_contexts_[i]->spsc_queue->pop();
        rmem::rt_assert(msg.status == 0 && msg.req_id == 0);
    }

    for (size_t i = 0; i < FLAGS_client_num; i++)
    {
        size_t tmp = FLAGS_concurrency;
        while (tmp--)
        {
            context->client_contexts_[i]->PushNextTCReq();
        }
    }
    while (1)
    {
        for (size_t i = 0; i < FLAGS_server_num; i++)
        {
            // connect success
            unsigned now_size = context->server_contexts_[i]->spsc_queue->was_size();
            while (now_size--)
            {
                RESP_MSG msg = context->server_contexts_[i]->spsc_queue->pop();
                printf("server %zu: status %d, req_id %u\n", i, msg.status, msg.req_id);

                // TODO maybe this i is not equal
                context->client_contexts_[i]->PushNextTCReq();
            }
        }
    }
}

int main(int argc, char **argv)
{
    signal(SIGINT, ctrl_c_handler);
    signal(SIGTERM, ctrl_c_handler);
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    check_common_gflags();

    std::thread leader_thread(leader_thread_func);
    rmem::bind_to_core(leader_thread, 1, get_bind_core(1));
    leader_thread.join();
}