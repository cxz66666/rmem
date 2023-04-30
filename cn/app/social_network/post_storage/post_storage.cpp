#include <thread>
#include "numautil.h"
#include "spinlock_mutex.h"
#include "post_storage.h"


void connect_sessions(ClientContext *c)
{

    // connect to backward server
    c->compose_post_session_num_ = c->rpc_->create_session(compose_post_addr, c->server_receiver_id_);
    rmem::rt_assert(c->compose_post_session_num_ >= 0, "Failed to create compose post session");

    c->user_timeline_session_num_ = c->rpc_->create_session(user_timeline_addr, c->server_receiver_id_);
    rmem::rt_assert(c->user_timeline_session_num_ >= 0, "Failed to create user timeline session");

    c->home_timeline_session_num_ = c->rpc_->create_session(home_timeline_addr, c->server_receiver_id_);
    rmem::rt_assert(c->home_timeline_session_num_ >= 0, "Failed to create home timeline session");

    while (c->num_sm_resps_ != 3)
    {
        c->rpc_->run_event_loop(kAppEvLoopMs);
        if (unlikely(ctrl_c_pressed == 1))
        {
            printf("Ctrl-C pressed. Exiting\n");
            return;
        }
    }
}

void ping_handler(erpc::ReqHandle *req_handle, void *_context)
{
    auto *ctx = static_cast<ServerContext *>(_context);
    ctx->stat_req_ping_tot++;
    auto *req_msgbuf = req_handle->get_req_msgbuf();
    rmem::rt_assert(req_msgbuf->get_data_size() == sizeof(RPCMsgReq<PingRPCReq>), "data size not match");

    auto *req = reinterpret_cast<RPCMsgReq<PingRPCReq> *>(req_msgbuf->buf_);

    new (req_handle->pre_resp_msgbuf_.buf_) RPCMsgResp<PingRPCResp>(req->req_common.type, req->req_common.req_number, 0, {req->req_control.timestamp});
    ctx->rpc_->resize_msg_buffer(&req_handle->pre_resp_msgbuf_, sizeof(RPCMsgResp<PingRPCResp>));

    ctx->init_mutex.lock();
    if(ctx->mongodb_init_finished){
        ctx->forward_all_mpmc_queue->push(*req_msgbuf);
        req_handle->get_hacked_req_msgbuf()->set_no_dynamic();
    } else {
        ctx->is_pinged = true;
    }
    ctx->init_mutex.unlock();

    ctx->rpc_->enqueue_response(req_handle, &req_handle->pre_resp_msgbuf_);
}

void post_storage_read_req_handler(erpc::ReqHandle *req_handler, void *_context)
{
    auto *ctx = static_cast<ServerContext *>(_context);
    ctx->stat_req_post_storage_read_tot++;

    ctx->init_mutex.lock();
    rmem::rt_assert(ctx->mongodb_init_finished,"mongodb not init finished!!");
    ctx->init_mutex.unlock();

    auto *req_msgbuf = req_handler->get_req_msgbuf();
    auto *req = reinterpret_cast<RPCMsgReq<CommonRPCReq> *>(req_msgbuf->buf_);

    rmem::rt_assert(req_msgbuf->get_data_size() == sizeof(RPCMsgReq<CommonRPCReq>) + req->req_control.data_length, "data size not match");

    new (req_handler->pre_resp_msgbuf_.buf_) RPCMsgResp<CommonRPCResp>(req->req_common.type, req->req_common.req_number, 0, {0});
    ctx->rpc_->resize_msg_buffer(&req_handler->pre_resp_msgbuf_, sizeof(RPCMsgResp<CommonRPCResp>));

    ctx->forward_all_mpmc_queue->push(*req_msgbuf);
    req_handler->get_hacked_req_msgbuf()->set_no_dynamic();

    ctx->rpc_->enqueue_response(req_handler, &req_handler->pre_resp_msgbuf_);

}

void post_storage_write_req_handler(erpc::ReqHandle *req_handler, void *_context)
{
    auto *ctx = static_cast<ServerContext *>(_context);
    ctx->stat_req_post_storage_write_tot++;

    ctx->init_mutex.lock();
    rmem::rt_assert(ctx->mongodb_init_finished,"mongodb not init finished!!");
    ctx->init_mutex.unlock();

    auto *req_msgbuf = req_handler->get_req_msgbuf();
    auto *req = reinterpret_cast<RPCMsgReq<CommonRPCReq> *>(req_msgbuf->buf_);

    rmem::rt_assert(req_msgbuf->get_data_size() == sizeof(RPCMsgReq<CommonRPCReq>) + req->req_control.data_length, "data size not match");

    new (req_handler->pre_resp_msgbuf_.buf_) RPCMsgResp<CommonRPCResp>(req->req_common.type, req->req_common.req_number, 0, {0});
    ctx->rpc_->resize_msg_buffer(&req_handler->pre_resp_msgbuf_, sizeof(RPCMsgResp<CommonRPCResp>));

    ctx->forward_all_mpmc_queue->push(*req_msgbuf);
    req_handler->get_hacked_req_msgbuf()->set_no_dynamic();

    ctx->rpc_->enqueue_response(req_handler, &req_handler->pre_resp_msgbuf_);

}

void callback_post_storage_write_resp(void *_context, void *_tag)
{
    auto req_id_ptr = reinterpret_cast<std::uintptr_t>(_tag);
    uint32_t req_id = req_id_ptr;
    auto *ctx = static_cast<ClientContext *>(_context);

    erpc::MsgBuffer &req_msgbuf = ctx->req_backward_msgbuf[req_id];
    erpc::MsgBuffer &resp_msgbuf = ctx->resp_backward_msgbuf[req_id];

    rmem::rt_assert(resp_msgbuf.get_data_size() == sizeof(RPCMsgResp<CommonRPCResp>), "data size not match");

    ctx->rpc_->free_msg_buffer(req_msgbuf);
}

void handler_post_storage_write_resp(ClientContext *ctx, const erpc::MsgBuffer &req_msgbuf)
{
    auto *req = reinterpret_cast<RPCMsgReq<CommonRPCReq> *>(req_msgbuf.buf_);

    ctx->req_backward_msgbuf[req->req_common.req_number % kAppMaxBuffer] = req_msgbuf;

    erpc::MsgBuffer &resp_msgbuf = ctx->resp_backward_msgbuf[req->req_common.req_number % kAppMaxBuffer];

    ctx->rpc_->enqueue_request(ctx->compose_post_session_num_, static_cast<uint8_t>(RPC_TYPE::RPC_POST_STORAGE_WRITE_RESP),
                               &ctx->req_backward_msgbuf[req->req_common.req_number % kAppMaxBuffer], &resp_msgbuf,
                               callback_post_storage_write_resp, reinterpret_cast<void *>(req->req_common.req_number % kAppMaxBuffer));

}

void callback_post_storage_read_resp(void *_context, void *_tag)
{
    auto req_id_ptr = reinterpret_cast<std::uintptr_t>(_tag);
    uint32_t req_id = req_id_ptr;
    auto *ctx = static_cast<ClientContext *>(_context);

    erpc::MsgBuffer &req_msgbuf = ctx->req_backward_msgbuf[req_id];
    erpc::MsgBuffer &resp_msgbuf = ctx->resp_backward_msgbuf[req_id];

    rmem::rt_assert(resp_msgbuf.get_data_size() == sizeof(RPCMsgResp<CommonRPCResp>), "data size not match");

    ctx->rpc_->free_msg_buffer(req_msgbuf);
}

void handler_post_storage_read_resp(ClientContext *ctx, const erpc::MsgBuffer &req_msgbuf)
{
    auto *req = reinterpret_cast<RPCMsgReq<CommonRPCReq> *>(req_msgbuf.buf_);

    ctx->req_backward_msgbuf[req->req_common.req_number % kAppMaxBuffer] = req_msgbuf;

    erpc::MsgBuffer &resp_msgbuf = ctx->resp_backward_msgbuf[req->req_common.req_number % kAppMaxBuffer];

    if(req_num_to_session_num[ctx->client_id_].count(req->req_common.req_number)) {
        int session_num =  req_num_to_session_num[ctx->client_id_][req->req_common.req_number];
        req_num_to_session_num[ctx->client_id_].erase(req->req_common.req_number);
        ctx->rpc_->enqueue_request(session_num, static_cast<uint8_t>(RPC_TYPE::RPC_POST_STORAGE_READ_RESP),
                                     &ctx->req_backward_msgbuf[req->req_common.req_number % kAppMaxBuffer], &resp_msgbuf,
                                   callback_post_storage_read_resp, reinterpret_cast<void *>(req->req_common.req_number % kAppMaxBuffer));

    } else {
        RMEM_WARN("req num %ud not found in req_num_to_session_num, thread is %ld", req->req_common.req_number, ctx->client_id_);
        ctx->rpc_->free_msg_buffer(req_msgbuf);
    }

}

void callback_ping_resp(void *_context, void *_tag)
{
    auto req_id_ptr = reinterpret_cast<std::uintptr_t>(_tag);
    uint32_t req_id = req_id_ptr;
    auto *ctx = static_cast<ClientContext *>(_context);

    // erpc::MsgBuffer &req_msgbuf = ctx->req_backward_msgbuf[req_id];
    erpc::MsgBuffer &resp_msgbuf = ctx->resp_backward_msgbuf[req_id];

    rmem::rt_assert(resp_msgbuf.get_data_size() == sizeof(RPCMsgResp<PingRPCResp>), "data size not match");

    // PingResp *resp = reinterpret_cast<PingResp *>(resp_msgbuf.buf_);

    // 如果返回值不为0，则认为后续不会有响应，直接将请求号和错误码放入队列
    // 如果返回值为0，则认为后续将有响应，不care
    // if (resp->resp.status != 0)
    // {
    // TODO
    // }

    // TODO check
    // ctx->rpc_->free_msg_buffer(req_msgbuf);
}

void handler_ping_resp(ClientContext *ctx, const erpc::MsgBuffer &req_msgbuf)
{

    auto *req = reinterpret_cast<RPCMsgReq<PingRPCReq> *>(req_msgbuf.buf_);

    ctx->req_backward_msgbuf[req->req_common.req_number % kAppMaxBuffer] = req_msgbuf;

    erpc::MsgBuffer &resp_msgbuf = ctx->resp_backward_msgbuf[req->req_common.req_number % kAppMaxBuffer];

    ctx->rpc_->enqueue_request(ctx->compose_post_session_num_, static_cast<uint8_t>(RPC_TYPE::RPC_PING_RESP),
                               &ctx->req_backward_msgbuf[req->req_common.req_number % kAppMaxBuffer], &resp_msgbuf,
                               callback_ping_resp, reinterpret_cast<void *>(req->req_common.req_number % kAppMaxBuffer));
}


void client_thread_func(size_t thread_id, ClientContext *ctx, erpc::Nexus *nexus)
{
    ctx->client_id_ = thread_id;
    std::vector<size_t> port_vec = flags_get_numa_ports(0);
    uint8_t phy_port = port_vec.at(thread_id % port_vec.size());

    uint8_t rpc_id;
#if defined(ERPC_PROGRAM)
    rpc_id = thread_id + FLAGS_server_num + kAppMaxRPC;
#elif defined(RMEM_PROGRAM)
    rpc_id = thread_id + FLAGS_server_num + kAppMaxRPC;
#endif
    erpc::Rpc<erpc::CTransport> rpc(nexus, static_cast<void *>(ctx),
                                    rpc_id,
                                    basic_sm_handler_client, phy_port);
    rpc.retry_connect_on_invalid_rpc_id_ = true;
    ctx->rpc_ = &rpc;
    for (auto & i : ctx->resp_backward_msgbuf)
    {
        // TODO
        i = rpc.alloc_msg_buffer_or_die(sizeof(RPCMsgResp<CommonRPCResp>));
    }

    connect_sessions(ctx);

    using FUNC_HANDLER = std::function<void(ClientContext *, erpc::MsgBuffer)>;
    std::map<RPC_TYPE ,FUNC_HANDLER > handlers{
            {RPC_TYPE::RPC_PING_RESP, handler_ping_resp},
            {RPC_TYPE::RPC_POST_STORAGE_WRITE_RESP, handler_post_storage_write_resp},
            {RPC_TYPE::RPC_POST_STORAGE_READ_RESP, handler_post_storage_read_resp},
    };

    while (true)
    {
        unsigned size = ctx->backward_mpmc_queue->was_size();
        for (unsigned i = 0; i < size; i++)
        {
            erpc::MsgBuffer req_msg = ctx->backward_mpmc_queue->pop();
            auto *req = reinterpret_cast<CommonReq *>(req_msg.buf_);
            rmem::rt_assert(req->type == RPC_TYPE::RPC_PING_RESP || req->type == RPC_TYPE::RPC_POST_STORAGE_READ_RESP
                            || req->type == RPC_TYPE::RPC_POST_STORAGE_WRITE_RESP, "only ping/post storage resp in backward queue");
            handlers[req->type](ctx, req_msg);
        }
        ctx->rpc_->run_event_loop_once();
        if (unlikely(ctrl_c_pressed))
        {
            break;
        }
    }
}

void server_thread_func(size_t thread_id, ServerContext *ctx, erpc::Nexus *nexus)
{
    ctx->server_id_ = thread_id;
    std::vector<size_t> port_vec = flags_get_numa_ports(0);
    uint8_t phy_port = port_vec.at(thread_id % port_vec.size());

    uint8_t rpc_id;
#if defined(ERPC_PROGRAM)
    rpc_id = thread_id + kAppMaxRPC;
#elif defined(RMEM_PROGRAM)
    rpc_id = thread_id + kAppMaxRPC;
#endif
    erpc::Rpc<erpc::CTransport> rpc(nexus, static_cast<void *>(ctx),
                                    rpc_id,
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
        printf("thread %zu: ping_req : %.2f, read/write : %.2f %.2f \n", thread_id,
               ctx->stat_req_ping_tot / seconds, ctx->stat_req_post_storage_read_tot / seconds, ctx->stat_req_post_storage_write_tot / seconds);

        ctx->rpc_->reset_dpath_stats();
        // more handler
        if (ctrl_c_pressed == 1)
        {
            break;
        }
    }
}
void worker_thread_func(size_t thread_id, MPMC_QUEUE *producer, MPMC_QUEUE *consumer, ClientContext *ctx, erpc::Rpc<erpc::CTransport> *server_rpc_)
{
    while (true)
    {
        unsigned size = producer->was_size();
        for (unsigned i = 0; i < size; i++)
        {
            erpc::MsgBuffer req_msg = producer->pop();

            auto *req = reinterpret_cast<CommonReq *>(req_msg.buf_);
            rmem::rt_assert(req->type == RPC_TYPE::RPC_PING || req->type == RPC_TYPE::RPC_POST_STORAGE_READ_REQ ||
                            req->type == RPC_TYPE::RPC_POST_STORAGE_WRITE_REQ, "req type error");

            if(req->type == RPC_TYPE::RPC_POST_STORAGE_READ_REQ) {
                read_post_storage(thread_id, req);
                server_rpc_->free_msg_buffer(req_msg);
            } else if(req->type == RPC_TYPE::RPC_POST_STORAGE_WRITE_REQ) {
                write_post_storage(thread_id, req,  ctx, consumer);
                server_rpc_->free_msg_buffer(req_msg);
            } else {
                req->type = RPC_TYPE::RPC_PING_RESP;
                consumer->push(req_msg);
            }
        }
        if (ctrl_c_pressed == 1)
        {
            break;
        }
    }
}
void reader_thread_func(size_t thread_id, MPMC_QUEUE *consumer, ClientContext* ctx) {
    std::queue<StorageHandler*> queues;
    size_t reading_posts = 0;

    StorageHandler *tmp = nullptr;
    const int max_batch = 16;
    int* batch_buffer = new int[max_batch];
    while(true) {

        //简单的流控
        if(reading_posts<kAppMaxBuffer){
            storage_queues[thread_id]->try_pop(tmp);
            if(tmp != nullptr) {
                queues.push(tmp);
                if(tmp->is_read){
                    for(size_t i=0;i<tmp->post_ids.size();i++){
                        rmems_[thread_id]->rmem_read_async(tmp->rmem_bufs[i], tmp->addrs_size[i].first, tmp->addrs_size[i].second);
                    }
                    reading_posts += tmp->post_ids.size();
                } else {
                    rmems_[thread_id]->rmem_write_async(tmp->rmem_bufs[0], tmp->addrs_size[0].first, tmp->addrs_size[0].second);
                    reading_posts += 1;
                }
            }
        }

        int fetch_num = rmems_[thread_id]->rmem_poll(batch_buffer, max_batch);
        reading_posts -= fetch_num;

        for(int i=0;i<fetch_num;i++){
            rmem::rt_assert(batch_buffer[i]==0, "read post error!");
        }

        while(fetch_num) {
            tmp = queues.front();
            if(tmp->is_read){
                size_t now_post_num = tmp->resp.posts_size();
                int parse_num = 0;
                for(int i=0;i<fetch_num && now_post_num+i< tmp->post_ids.size();i++){
                    tmp->resp.add_posts()->ParseFromArray(tmp->rmem_bufs[i + now_post_num], tmp->addrs_size[i + now_post_num].second);
                    parse_num++;
                }

                if(tmp->resp.posts_size() == static_cast<int>(tmp->post_ids.size())){
                    size_t post_serialize_size = tmp->resp.ByteSizeLong();
                    erpc::MsgBuffer resp_buf = ctx->rpc_->alloc_msg_buffer_or_die(sizeof(RPCMsgReq<CommonRPCReq>) + post_serialize_size);
                    auto* resp_buf_msg = new (resp_buf.buf_) RPCMsgReq<CommonRPCReq>(RPC_TYPE::RPC_POST_STORAGE_READ_RESP, tmp->req_number);
                    tmp->resp.SerializeToArray(resp_buf_msg+1, post_serialize_size);

                    if(tmp->rpc_type == static_cast<uint32_t>(RPC_TYPE::RPC_USER_TIMELINE_READ_REQ)){
                        req_num_to_session_num[thread_id][tmp->req_number] = ctx->user_timeline_session_num_;
                    } else if(tmp->rpc_type == static_cast<uint32_t>(RPC_TYPE::RPC_HOME_TIMELINE_READ_REQ)) {
                        req_num_to_session_num[thread_id][tmp->req_number] = ctx->home_timeline_session_num_;
                    } else {
                        RMEM_ERROR("rpc type error!");
                        exit(-1);
                    }
                    consumer->push(resp_buf);

                    for(auto & read_buf : tmp->rmem_bufs){
                        rmems_[thread_id]->rmem_free_msg_buffer(read_buf);
                    }
                    delete tmp;
                    queues.pop();
                }
                fetch_num -= parse_num;
            } else {
                rmems_[thread_id]->rmem_free_msg_buffer(tmp->rmem_bufs[0]);
                delete tmp;
                queues.pop();
                fetch_num -= 1;
            }
        }
    }

}
void leader_thread_func()
{
    erpc::Nexus nexus(rmem::extract_hostname_from_uri(FLAGS_server_addr),
                      rmem::extract_udp_port_from_uri(FLAGS_server_addr), 0);

    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_PING), ping_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_POST_STORAGE_READ_REQ), post_storage_read_req_handler);
    nexus.register_req_func(static_cast<uint8_t>(RPC_TYPE::RPC_POST_STORAGE_WRITE_REQ), post_storage_write_req_handler);

    std::vector<std::thread> clients(FLAGS_client_num);
    std::vector<std::thread> servers(FLAGS_server_num);
    std::vector<std::thread> workers(FLAGS_client_num);
    std::vector<std::thread> readers(FLAGS_client_num);

    auto *context = new AppContext();

    clients[0] = std::thread(client_thread_func, 0, context->client_contexts_[0], &nexus);
    sleep(2);
    rmem::bind_to_core(clients[0], FLAGS_numa_client_node, get_bind_core(FLAGS_numa_client_node) + FLAGS_bind_core_offset);

    for (size_t i = 1; i < FLAGS_client_num; i++)
    {
        clients[i] = std::thread(client_thread_func, i, context->client_contexts_[i], &nexus);

        rmem::bind_to_core(clients[i], FLAGS_numa_client_node, get_bind_core(FLAGS_numa_client_node) + FLAGS_bind_core_offset);
    }

    for (size_t i = 0; i < FLAGS_server_num; i++)
    {
        servers[i] = std::thread(server_thread_func, i, context->server_contexts_[i], &nexus);

        rmem::bind_to_core(servers[i], FLAGS_numa_server_node, get_bind_core(FLAGS_numa_server_node) + FLAGS_bind_core_offset);
    }
    sleep(3);

    for (size_t i = 0; i < FLAGS_client_num; i++)
    {
        rmem::rt_assert(context->server_contexts_[i]->rpc_ != nullptr && context->server_contexts_[i]->rpc_ != nullptr, "server rpc is null");
        workers[i] = std::thread(worker_thread_func, i, context->client_contexts_[i]->forward_all_mpmc_queue, context->client_contexts_[i]->backward_mpmc_queue,
                                 context->client_contexts_[i], context->server_contexts_[i]->rpc_);
        rmem::bind_to_core(clients[i], FLAGS_numa_client_node, get_bind_core(FLAGS_numa_client_node) + FLAGS_bind_core_offset);

    }
    for(size_t i=0; i< FLAGS_client_num; i++)
    {
        readers[i] = std::thread(reader_thread_func, i, context->client_contexts_[i]->backward_mpmc_queue, context->client_contexts_[i]);
        rmem::bind_to_core(clients[i], FLAGS_numa_client_node, get_bind_core(FLAGS_numa_client_node) + FLAGS_bind_core_offset);
    }

    sleep(2);
    if (FLAGS_timeout_second != UINT64_MAX)
    {
        sleep(FLAGS_timeout_second);
        ctrl_c_pressed = true;
    }

    for (size_t i = 0; i < FLAGS_client_num; i++)
    {
        clients[i].join();
    }
    for (size_t i = 0; i < FLAGS_server_num; i++)
    {
        servers[i].join();
    }
    for (size_t i = 0; i < FLAGS_client_num; i++)
    {
        workers[i].join();
    }
    for (size_t i = 0; i < FLAGS_client_num; i++)
    {
        readers[i].join();
    }
}

int main(int argc, char **argv)
{

    signal(SIGINT, ctrl_c_handler);
    signal(SIGTERM, ctrl_c_handler);
    // only config_file is required!!!
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    init_service_config(FLAGS_config_file,"post_storage");
    init_specific_config();

    std::thread leader_thread(leader_thread_func);
    rmem::bind_to_core(leader_thread, 1, get_bind_core(1));
    leader_thread.join();
}