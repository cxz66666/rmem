#include "worker.h"
#include "rpc_type.h"
#include "req_type.h"
namespace rmem
{
    void worker_func(Context *ctx)
    {
        rt_assert(ctx != nullptr);
        rt_assert(ctx->rpc_ != nullptr);

        WorkerStore *ws = new WorkerStore();

        using RMEM_HANDLER = std::function<void(Context * ctx, WorkerStore * ws, const RingBufElement &el)>;

        RMEM_HANDLER rmem_handlers[] = {handler_connect, handler_disconnnect, handler_alloc, handler_free,
                                        handler_read_sync, handler_read_async, handler_write_sync,
                                        handler_write_async, handler_poll};

        auto handler = [&](RingBufElement const el) -> void
        {
            rt_assert(el.req_type >= REQ_TYPE::RMEM_CONNECT);
            rt_assert(el.req_type <= REQ_TYPE::RMEM_POOL);
            rmem_handlers[static_cast<uint8_t>(el.req_type)](ctx, ws, el);
        };
        while (1)
        {
            RingBuf_process_all(ctx->ringbuf_, handler);
            ctx->rpc_->run_event_loop_once();
            if (ctx->worker_stop_)
            {
                RMEM_INFO("worker thread exit");
                break;
            }
        }

        delete ws;
    }

    void handler_connect(Context *ctx, WorkerStore *ws, const RingBufElement &el)
    {
        _unused(ws);
        rt_assert(ctx->rpc_->num_active_sessions() == 0, "one context can only have one connected session");
        rt_assert(ctx->concurrent_store_->get_session_num() == -1, "session num must be zero");
        int session_num = ctx->rpc_->create_session(std::string(el.connect.host), el.connect.remote_rpc_id);
        rt_assert(session_num >= 0, "get a negative session num");
    }
    void handler_disconnnect(Context *ctx, WorkerStore *ws, const RingBufElement &el)
    {
        _unused(el);
        _unused(ws);
        rt_assert(ctx->rpc_->num_active_sessions() == 1, "one context can only disconnect after connnected");
        rt_assert(ctx->concurrent_store_->get_session_num() != -1, "session num must be 1");

        int res = ctx->rpc_->destroy_session(ctx->concurrent_store_->get_session_num());
        if (res != 0)
        {
            ctx->condition_resp_->notify_waiter(res, "");
            return;
        }
        // TODO  if res==0, then we will send a disconnect request, we will clear session at sm_handler
    }
    void handler_alloc(Context *ctx, WorkerStore *ws, const RingBufElement &el)
    {
        size_t req_number = ws->generate_next_num();
        erpc::MsgBuffer req = ctx->rpc_->alloc_msg_buffer_or_die(sizeof(AllocReq));
        erpc::MsgBuffer resp = ctx->rpc_->alloc_msg_buffer_or_die(sizeof(AllocResp));

        new (req.buf_) AllocReq(RPC_TYPE::RPC_ALLOC, req_number, el.alloc.alloc_size, el.alloc.vm_flags);
        ws->sended_req[req_number] = {req, resp};

        ctx->rpc_->enqueue_request(ctx->concurrent_store_->get_session_num(), static_cast<uint8_t>(RPC_TYPE::RPC_ALLOC),
                                   &ws->sended_req[req_number].first, &ws->sended_req[req_number].second,
                                   callback_alloc, reinterpret_cast<void *>(new WorkerTag{ws, req_number}));
    }
    void handler_free(Context *ctx, WorkerStore *ws, const RingBufElement &el)
    {
        size_t req_number = ws->generate_next_num();
        erpc::MsgBuffer req = ctx->rpc_->alloc_msg_buffer_or_die(sizeof(FreeReq));
        erpc::MsgBuffer resp = ctx->rpc_->alloc_msg_buffer_or_die(sizeof(FreeReq));
        new (req.buf_) FreeReq(RPC_TYPE::RPC_FREE, req_number, el.alloc.alloc_addr, el.alloc.alloc_size);
        ws->sended_req[req_number] = {req, resp};
        ctx->rpc_->enqueue_request(ctx->concurrent_store_->get_session_num(), static_cast<uint8_t>(RPC_TYPE::RPC_FREE),
                                   &ws->sended_req[req_number].first, &ws->sended_req[req_number].second,
                                   callback_free, reinterpret_cast<void *>(new WorkerTag{ws, req_number}));
    }
    void handler_read_sync(Context *ctx, WorkerStore *ws, const RingBufElement &el)
    {
        size_t req_number = ws->generate_next_num();
        erpc::MsgBuffer req = ctx->rpc_->alloc_msg_buffer_or_die(sizeof(ReadReq));
        erpc::MsgBuffer resp = ctx->rpc_->alloc_msg_buffer_or_die(sizeof(ReadResp) + sizeof(char) * el.rw.rw_size);
        new (req.buf_) ReadReq(RPC_TYPE::RPC_READ, req_number, el.rw.rw_buffer, el.rw.rw_addr, el.rw.rw_size);
        ws->sended_req[req_number] = {req, resp};
        ctx->rpc_->enqueue_request(ctx->concurrent_store_->get_session_num(), static_cast<uint8_t>(RPC_TYPE::RPC_READ),
                                   &ws->sended_req[req_number].first, &ws->sended_req[req_number].second,
                                   callback_read_sync, reinterpret_cast<void *>(new WorkerTag{ws, req_number}));
    }
    void handler_read_async(Context *ctx, WorkerStore *ws, const RingBufElement &el)
    {
        size_t req_number = ws->generate_next_num();
        erpc::MsgBuffer req = ctx->rpc_->alloc_msg_buffer_or_die(sizeof(ReadReq));
        erpc::MsgBuffer resp = ctx->rpc_->alloc_msg_buffer_or_die(sizeof(ReadResp) + sizeof(char) * el.rw.rw_size);
        new (req.buf_) ReadReq(RPC_TYPE::RPC_READ, req_number, el.rw.rw_buffer, el.rw.rw_addr, el.rw.rw_size);
        ws->sended_req[req_number] = {req, resp};
        ws->async_received_req[req_number] = INT_MAX;
        ctx->rpc_->enqueue_request(ctx->concurrent_store_->get_session_num(), static_cast<uint8_t>(RPC_TYPE::RPC_READ),
                                   &ws->sended_req[req_number].first, &ws->sended_req[req_number].second,
                                   callback_read_async, reinterpret_cast<void *>(new WorkerTag{ws, req_number}));
    }
    void handler_write_sync(Context *ctx, WorkerStore *ws, const RingBufElement &el)
    {
        size_t req_number = ws->generate_next_num();
        erpc::MsgBuffer req = ctx->rpc_->alloc_msg_buffer_or_die(sizeof(WriteReq) + sizeof(char) * el.rw.rw_size);
        erpc::MsgBuffer resp = ctx->rpc_->alloc_msg_buffer_or_die(sizeof(WriteResp));
        new (req.buf_) WriteReq(RPC_TYPE::RPC_WRITE, req_number, el.rw.rw_addr, el.rw.rw_size);
        memcpy(req.buf_ + sizeof(WriteReq), el.rw.rw_buffer, el.rw.rw_size);

        ws->sended_req[req_number] = {req, resp};
        ctx->rpc_->enqueue_request(ctx->concurrent_store_->get_session_num(), static_cast<uint8_t>(RPC_TYPE::RPC_WRITE),
                                   &ws->sended_req[req_number].first, &ws->sended_req[req_number].second,
                                   callback_write_sync, reinterpret_cast<void *>(new WorkerTag{ws, req_number}));
    }
    void handler_write_async(Context *ctx, WorkerStore *ws, const RingBufElement &el)
    {
        size_t req_number = ws->generate_next_num();
        erpc::MsgBuffer req = ctx->rpc_->alloc_msg_buffer_or_die(sizeof(WriteReq) + sizeof(char) * el.rw.rw_size);
        erpc::MsgBuffer resp = ctx->rpc_->alloc_msg_buffer_or_die(sizeof(WriteResp));
        new (req.buf_) WriteReq(RPC_TYPE::RPC_WRITE, req_number, el.rw.rw_addr, el.rw.rw_size);
        memcpy(req.buf_ + sizeof(WriteReq), el.rw.rw_buffer, el.rw.rw_size);

        ws->sended_req[req_number] = {req, resp};
        ws->async_received_req[req_number] = INT_MAX;

        ctx->rpc_->enqueue_request(ctx->concurrent_store_->get_session_num(), static_cast<uint8_t>(RPC_TYPE::RPC_WRITE),
                                   &ws->sended_req[req_number].first, &ws->sended_req[req_number].second,
                                   callback_write_async, reinterpret_cast<void *>(new WorkerTag{ws, req_number}));
    }

    void handler_fork(Context *ctx, WorkerStore *ws, const RingBufElement &el)
    {
        size_t req_number = ws->generate_next_num();
        erpc::MsgBuffer req = ctx->rpc_->alloc_msg_buffer_or_die(sizeof(ForkReq));
        erpc::MsgBuffer resp = ctx->rpc_->alloc_msg_buffer_or_die(sizeof(ForkResp));

        new (req.buf_) ForkReq(RPC_TYPE::RPC_FORK, req_number, el.alloc.alloc_addr, el.alloc.alloc_size, el.alloc.vm_flags);
        ws->sended_req[req_number] = {req, resp};

        ctx->rpc_->enqueue_request(ctx->concurrent_store_->get_session_num(), static_cast<uint8_t>(RPC_TYPE::RPC_FORK),
                                   &ws->sended_req[req_number].first, &ws->sended_req[req_number].second,
                                   callback_fork, reinterpret_cast<void *>(new WorkerTag{ws, req_number}));
    }

    void handler_poll(Context *ctx, WorkerStore *ws, const RingBufElement &el)
    {
        int num = 0;
        for (auto m = ws->async_received_req.begin(); m != ws->async_received_req.end();)
        {

            if (m->second == INT_MAX)
            {
                break;
            }
            el.poll.poll_results[num++] = m->second;
            // warning: iter loss effect if ++ iter after erase
            ws->async_received_req.erase(m++);

            if (num == el.poll.poll_max_num)
            {
                break;
            }
        }
        RMEM_INFO("polled %d response (max num %d)", num, el.poll.poll_max_num);

        ctx->condition_resp_->notify_waiter(num, "");
        return;
    }

    void callback_alloc(void *_context, void *_tag)
    {
        Context *ctx = static_cast<Context *>(_context);
        WorkerTag *worker_tag = static_cast<WorkerTag *>(_tag);
        WorkerStore *ws = worker_tag->ws;
        size_t req_number = worker_tag->req_number;
        rt_assert(ws != nullptr, "worker store must not be empty!");

        rt_assert(ws->sended_req.count(req_number) > 0, "sended_req must have req entry");

        erpc::MsgBuffer req_buffer = ws->sended_req[req_number].first;
        erpc::MsgBuffer resp_buffer = ws->sended_req[req_number].second;

        rt_assert(resp_buffer.get_data_size() == sizeof(AllocResp));

        AllocResp *resp = reinterpret_cast<AllocResp *>(resp_buffer.buf_);

        // TODO add some debug msg
        ctx->condition_resp_->notify_waiter_extra(resp->resp.status, resp->raddr, "");

        ctx->rpc_->free_msg_buffer(req_buffer);
        ctx->rpc_->free_msg_buffer(resp_buffer);
        ws->sended_req.erase(req_number);
    }
    void callback_free(void *_context, void *_tag)
    {
        Context *ctx = static_cast<Context *>(_context);
        WorkerTag *worker_tag = static_cast<WorkerTag *>(_tag);
        WorkerStore *ws = worker_tag->ws;
        size_t req_number = worker_tag->req_number;
        rt_assert(ws != nullptr, "worker store must not be empty!");

        rt_assert(ws->sended_req.count(req_number) > 0, "sended_req must have req entry");

        erpc::MsgBuffer req_buffer = ws->sended_req[req_number].first;
        erpc::MsgBuffer resp_buffer = ws->sended_req[req_number].second;

        rt_assert(resp_buffer.get_data_size() == sizeof(FreeResp));

        FreeResp *resp = reinterpret_cast<FreeResp *>(resp_buffer.buf_);

        // TODO add some debug msg
        ctx->condition_resp_->notify_waiter(resp->resp.status, "");

        ctx->rpc_->free_msg_buffer(req_buffer);
        ctx->rpc_->free_msg_buffer(resp_buffer);
        ws->sended_req.erase(req_number);
    }
    void callback_read_async(void *_context, void *_tag)
    {
        Context *ctx = static_cast<Context *>(_context);
        WorkerTag *worker_tag = static_cast<WorkerTag *>(_tag);
        WorkerStore *ws = worker_tag->ws;
        size_t req_number = worker_tag->req_number;
        rt_assert(ws != nullptr, "worker store must not be empty!");

        rt_assert(ws->sended_req.count(req_number) > 0, "sended_req must have req entry");

        erpc::MsgBuffer req_buffer = ws->sended_req[req_number].first;
        erpc::MsgBuffer resp_buffer = ws->sended_req[req_number].second;

        ReadResp *resp = reinterpret_cast<ReadResp *>(resp_buffer.buf_);

        rt_assert(resp_buffer.get_data_size() == sizeof(ReadResp) + sizeof(char) * resp->rsize);

        // TODO enhance this copy!
        memcpy(resp->recv_buf, resp + 1, resp->rsize);

        // TODO add some debug msg
        ws->async_received_req[req_number] = resp->resp.status;

        ctx->rpc_->free_msg_buffer(req_buffer);
        ctx->rpc_->free_msg_buffer(resp_buffer);
        ws->sended_req.erase(req_number);
    }
    void callback_read_sync(void *_context, void *_tag)
    {
        Context *ctx = static_cast<Context *>(_context);
        WorkerTag *worker_tag = static_cast<WorkerTag *>(_tag);
        WorkerStore *ws = worker_tag->ws;
        size_t req_number = worker_tag->req_number;
        rt_assert(ws != nullptr, "worker store must not be empty!");

        rt_assert(ws->sended_req.count(req_number) > 0, "sended_req must have req entry");

        erpc::MsgBuffer req_buffer = ws->sended_req[req_number].first;
        erpc::MsgBuffer resp_buffer = ws->sended_req[req_number].second;

        ReadResp *resp = reinterpret_cast<ReadResp *>(resp_buffer.buf_);

        rt_assert(resp_buffer.get_data_size() == sizeof(ReadResp) + sizeof(char) * resp->rsize);

        // TODO enhance this copy!
        memcpy(resp->recv_buf, resp + 1, resp->rsize);

        // TODO add some debug msg
        ctx->condition_resp_->notify_waiter(resp->resp.status, "");

        ctx->rpc_->free_msg_buffer(req_buffer);
        ctx->rpc_->free_msg_buffer(resp_buffer);
        ws->sended_req.erase(req_number);
    }
    void callback_write_async(void *_context, void *_tag)
    {
        Context *ctx = static_cast<Context *>(_context);
        WorkerTag *worker_tag = static_cast<WorkerTag *>(_tag);
        WorkerStore *ws = worker_tag->ws;
        size_t req_number = worker_tag->req_number;
        rt_assert(ws != nullptr, "worker store must not be empty!");

        rt_assert(ws->sended_req.count(req_number) > 0, "sended_req must have req entry");

        erpc::MsgBuffer req_buffer = ws->sended_req[req_number].first;
        erpc::MsgBuffer resp_buffer = ws->sended_req[req_number].second;

        WriteResp *resp = reinterpret_cast<WriteResp *>(resp_buffer.buf_);

        rt_assert(resp_buffer.get_data_size() == sizeof(WriteResp));

        // TODO add some debug msg
        ws->async_received_req[req_number] = resp->resp.status;

        ctx->rpc_->free_msg_buffer(req_buffer);
        ctx->rpc_->free_msg_buffer(resp_buffer);
        ws->sended_req.erase(req_number);
    }
    void callback_write_sync(void *_context, void *_tag)
    {
        Context *ctx = static_cast<Context *>(_context);
        WorkerTag *worker_tag = static_cast<WorkerTag *>(_tag);
        WorkerStore *ws = worker_tag->ws;
        size_t req_number = worker_tag->req_number;
        rt_assert(ws != nullptr, "worker store must not be empty!");

        rt_assert(ws->sended_req.count(req_number) > 0, "sended_req must have req entry");

        erpc::MsgBuffer req_buffer = ws->sended_req[req_number].first;
        erpc::MsgBuffer resp_buffer = ws->sended_req[req_number].second;

        WriteResp *resp = reinterpret_cast<WriteResp *>(resp_buffer.buf_);

        rt_assert(resp_buffer.get_data_size() == sizeof(WriteResp));

        // TODO add some debug msg

        ctx->condition_resp_->notify_waiter(resp->resp.status, "");

        ctx->rpc_->free_msg_buffer(req_buffer);
        ctx->rpc_->free_msg_buffer(resp_buffer);
        ws->sended_req.erase(req_number);
    }

    void callback_fork(void *_context, void *_tag)
    {
        Context *ctx = static_cast<Context *>(_context);
        WorkerTag *worker_tag = static_cast<WorkerTag *>(_tag);
        WorkerStore *ws = worker_tag->ws;
        size_t req_number = worker_tag->req_number;
        rt_assert(ws != nullptr, "worker store must not be empty!");

        rt_assert(ws->sended_req.count(req_number) > 0, "sended_req must have req entry");

        erpc::MsgBuffer req_buffer = ws->sended_req[req_number].first;
        erpc::MsgBuffer resp_buffer = ws->sended_req[req_number].second;

        ForkResp *resp = reinterpret_cast<ForkResp *>(resp_buffer.buf_);

        rt_assert(resp_buffer.get_data_size() == sizeof(WriteResp));

        ctx->condition_resp_->notify_waiter(resp->resp.status, "");

        ctx->rpc_->free_msg_buffer(req_buffer);
        ctx->rpc_->free_msg_buffer(resp_buffer);
        ws->sended_req.erase(req_number);
    }

    void basic_sm_handler(int session_num, erpc::SmEventType sm_event_type,
                          erpc::SmErrType sm_err_type, void *_context)
    {
        Context *ctx = static_cast<Context *>(_context);

        switch (sm_event_type)
        {
        case erpc::SmEventType::kConnected:
        {

            RMEM_INFO("Connect connected %d.\n", session_num);
            // TODO add timeout handler
            rt_assert(sm_err_type == erpc::SmErrType::kNoError);
            ctx->concurrent_store_->insert_session(session_num);
            ctx->condition_resp_->notify_waiter(static_cast<int>(sm_err_type), "");
            break;
        }
        case erpc::SmEventType::kConnectFailed:
        {
            RMEM_WARN("Connect Error %s.\n",
                      sm_err_type_str(sm_err_type).c_str());
            ctx->condition_resp_->notify_waiter(static_cast<int>(sm_err_type), "");
            break;
        }
        case erpc::SmEventType::kDisconnected:
        {
            RMEM_INFO("Connect disconnected %d.\n", session_num);
            rt_assert(sm_err_type == erpc::SmErrType::kNoError);

            ctx->concurrent_store_->clear_session();
            ctx->condition_resp_->notify_waiter(static_cast<int>(sm_err_type), "");
            break;
        }
        case erpc::SmEventType::kDisconnectFailed:
        {
            RMEM_WARN("Connect Error %s.\n",
                      sm_err_type_str(sm_err_type).c_str());
            ctx->condition_resp_->notify_waiter(static_cast<int>(sm_err_type), "");
            break;
        }
        }
    }
}