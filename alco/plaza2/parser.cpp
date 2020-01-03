/*
   author: Ilya Andronov <sni4ok@yandex.ru>
*/

#include "config.hpp"
#include "plaza_common.hpp"

#include "../alco.hpp"

#include "scheme/instruments.inl"
#include "scheme/orders.inl"
#include "scheme/trades.inl"

#include <unistd.h>

CG_RESULT instruments_callback(cg_conn_t* conn, cg_listener_t* listener, struct cg_msg_t* msg, void* p);
CG_RESULT orders_callback(cg_conn_t* conn, cg_listener_t* listener, struct cg_msg_t* msg, void* p);
CG_RESULT trades_callback(cg_conn_t* conn, cg_listener_t* listener, struct cg_msg_t* msg, void* p);

struct parser : stack_singleton<parser>
{
    fmap<int32_t, security> tickers;
    tyra t;
    volatile bool tickers_initialized;

    cg_conn_h conn;
    cg_listener_h orders, trades;
    
    void init_tickers(volatile bool& can_run, const fmap<int32_t, security>& ft)
    {
        conn.connect(can_run);
        for(auto& v: ft)
            t.send(v.second.mi);
        tickers = ft;
        t.flush();
    }
    void init_tickers(volatile bool& can_run)
    {
        cg_listener_h instruments(conn, "instruments", cfg_cli_instruments, instruments_callback, &tickers);
        conn.connect(can_run);
        instruments.open();
        mlog() << "instruments successfully connected to p2router";

        while(!tickers_initialized && can_run) {
            instruments.reopen();
            int r = cg_conn_process(conn.cli, 1, 0);
            if(unlikely(r != CG_ERR_TIMEOUT && r != CG_ERR_OK)){
                mlog() << "parser failed to process connection: " << r;
                instruments.destroy();
            }
            if(unlikely(r == CG_ERR_TIMEOUT)){
                instruments.reopen_unactive();
            }
        }
        mlog() << "parser::init_tickers() ended, initialized " << tickers.size() << " securities";
    }
    void clear_order_book()
    {
        auto it = tickers.begin(), it_e = tickers.end();
        for(; it != it_e; ++it)
            it->second.proceed_clean(t);
        t.flush();
    }
    parser() : t(config::instance().push), tickers_initialized(), conn(config::instance().cli_conn_recv),
        orders(conn, "orders", cfg_cli_orders, orders_callback, &tickers),
        trades(conn, "trades", cfg_cli_trades, trades_callback, &tickers)
    {
        std::string env = "ini=./plaza_recv.ini;key=" + config::instance().key;
        mlog() << "cg_env_open " << env;
        check_plaza_fail(cg_env_open(env.c_str()), "env_open");
    }

    void disconnect()
    {
        trades.destroy();
        orders.destroy();
        conn.disconnect();
    }
    void connect(volatile bool& can_run)
    {
        if(!can_run)
            return;
        conn.connect(can_run);
        orders.open();
        trades.open();
    }
    void proceed(volatile bool& can_run)
    {
        for(uint32_t c = 0; can_run; ++c) {
            if(unlikely(!conn.cli)) {
                disconnect();
                connect(can_run);
                if(!can_run)
                    break;
            }
            orders.reopen();
            trades.reopen();
            int r = cg_conn_process(conn.cli, 1, 0);
            if(unlikely(r != CG_ERR_TIMEOUT && r != CG_ERR_OK)) {
                mlog() << "parser failed to process connection: " << r;
                disconnect();
                connect(can_run);
            }
            if(unlikely(r == CG_ERR_TIMEOUT)){
                orders.reopen_unactive();
                trades.reopen_unactive();
            }
            //if(!(c % 10000))
            //    parser::instance().t.ping();
        }
    }
    ~parser()
    {
        warn_plaza_fail(cg_env_close(), "env_close");
    }
};

void proceed_plaza(volatile bool& can_run)
{
    fmap<int32_t, security> tickers;
    while(can_run) {
        try {
            parser p;
            if(tickers.empty()) {
                p.init_tickers(can_run);
                tickers = p.tickers;
            }
            else
                p.init_tickers(can_run, tickers);
            p.proceed(can_run);
        }
        catch(std::exception& e) {
            mlog() << "proceed_plaza " << e;
            if(can_run)
                usleep(5000 * 1000);
        }
    }
}

CG_RESULT instruments_callback(cg_conn_t*, cg_listener_t*, struct cg_msg_t* msg, void* p)
{
    switch (msg->type)
    {
    case CG_MSG_STREAM_DATA: 
        {
            config& c= config::instance();
            if(msg->data_size == sizeof(fut_sess_contents)){
                fut_sess_contents* f = (fut_sess_contents*)msg->data;
                std::string ticker(f->short_isin.buf);
                if(config::instance().proceed_ticker(ticker)) {
                    fmap<int32_t, security>& tickers = *((fmap<int32_t, security>*)p);
                    if(c.log_plaza)
                        f->print_brief();
                    auto it = tickers.find(f->isin_id);
                    if(it == tickers.end()) {
                        security& sec = tickers[f->isin_id];
                        sec.init(c.exchange_id, c.feed_id, std::string(f->short_isin.buf));
                        parser::instance().t.send(sec.mi);
                    }
                    else {
                        mlog() << "security " << f->isin_id << ", " << std::string(f->short_isin.buf) << " already initialized";
                        parser::instance().tickers_initialized = true;
                    }
                }
            }

            break;
        }
    case CG_MSG_TN_BEGIN:
        {
            //mlog() << "instruments_callback CG_MSG_TN_BEGIN";
            break;
        } 
    case CG_MSG_TN_COMMIT:
        {
            //mlog() << "instruments_callback CG_MSG_TN_COMMIT";
            break;
        } 
    case CG_MSG_P2REPL_CLEARDELETED:
        {
            if(config::instance().log_plaza)
                mlog() << "instruments_callback CG_MSG_P2REPL_CLEARDELETED";
            break;
        } 
    case CG_MSG_P2REPL_LIFENUM:
        {
            if(config::instance().log_plaza)
                mlog() << "instruments_callback CG_MSG_P2REPL_LIFENUM, data_size: " << msg->data_size << ", lnum: " << *((int*)msg->data);
            break;
        } 
    case CG_MSG_OPEN:
        {
            if(config::instance().log_plaza)
                mlog() << "instruments_callback cg_msg_open";
            break;
        } 
    case CG_MSG_CLOSE:
        {
            if(config::instance().log_plaza)
                mlog() << "instruments_callback cg_msg_close";
            break;
        } 
    case CG_MSG_P2MQ_TIMEOUT: 
        {
            if(config::instance().log_plaza)
                mlog(mlog::critical) << "instruments_callback plaza timeout";
            break;
        }
    case CG_MSG_P2REPL_ONLINE: 
        {
            if(config::instance().log_plaza)
                mlog() << "instruments_callback plaza online setted";
            parser::instance().tickers_initialized = true;
            break;
        }
    default:
        if(config::instance().log_plaza)
            mlog(mlog::critical) << "instruments_callback unknown message type: " << msg->type;
    }
    return CG_ERR_OK;
}

CG_RESULT orders_callback(cg_conn_t*, cg_listener_t*, struct cg_msg_t* msg, void* p)
{
    switch (msg->type)
    {
    case CG_MSG_STREAM_DATA: 
        {
            if(unlikely(msg->data_size != sizeof(orders_aggr))){
                throw std::runtime_error("proceed_order not orders_aggr received");
            }
            orders_aggr* o = (orders_aggr*)msg->data;
            fmap<int32_t, security>& tickers = *((fmap<int32_t, security>*)p);
            auto it = tickers.find(o->isin_id);
            if(it == tickers.end()) {
                break;
            }
            security& s = it->second;
            price_t price = *o->price;
            count_t count = {o->volume * count_t::frac};
            ttime_t etime = {o->moment_ns};
            if(o->dir == 2)
                count.value = -count.value;
            else if(likely(o->dir == 1))
            {
            }
            else {
                o->print_brief();
                throw std::runtime_error("orders_aggr bad dir");
            }
            s.proceed_book(parser::instance().t, price, count, etime);
            break;
        }
    case CG_MSG_TN_BEGIN:
        {
            //market_data_manager::instance().check_heartbeat();
            //mlog() << "orders_callback CG_MSG_TN_BEGIN";
            break;
        } 
    case CG_MSG_TN_COMMIT:
        {
            parser::instance().t.flush();
            if(config::instance().log_plaza)
                mlog() << "orders_callback CG_MSG_TN_COMMIT";
            break;
        } 
    case CG_MSG_P2REPL_CLEARDELETED:
        {
            parser::instance().clear_order_book();
            cg_data_cleardeleted_t* p = (cg_data_cleardeleted_t*)msg->data;
            if(config::instance().log_plaza)
                mlog() << "orders_callback CG_MSG_P2REPL_CLEARDELETED table_idx: " << p->table_idx << ", table_rev: " << p->table_rev;
            break;
        } 
    case CG_MSG_P2REPL_LIFENUM:
        {
            if(config::instance().log_plaza)
                mlog() << "orders_callback CG_MSG_P2REPL_LIFENUM, data_size: " << msg->data_size << ", lnum: " << *((int*)msg->data);
            break;
        } 
    case CG_MSG_OPEN:
        {
            if(config::instance().log_plaza)
                mlog() << "orders_callback cg_msg_open";
            break;
        } 
    case CG_MSG_CLOSE:
        {
            if(config::instance().log_plaza)
                mlog() << "orders_callback cg_msg_close";
            parser::instance().orders.set_closed();
            break;
        } 
    case CG_MSG_P2MQ_TIMEOUT: 
        {
            if(config::instance().log_plaza)
                mlog(mlog::critical) << "orders_callback plaza timeout";
            break;
        }
    case CG_MSG_P2REPL_REPLSTATE: 
        {
            if(config::instance().log_plaza)
                mlog() << "orders_callback CG_MSG_P2REPL_REPLSTATE, data, str: " << str_holder((char*)msg->data);
            parser::instance().orders.set_replstate(msg);
            break;
        }
    case CG_MSG_P2REPL_ONLINE: 
        {
            mlog() << "orders_callback plaza online setted";
            break;
        }
    default:
        if(config::instance().log_plaza)
            mlog(mlog::critical) << "orders_callback unknown message type: " << msg->type;
    }
    parser::instance().orders.set_call();
    return CG_ERR_OK;
}

bool deals_online = false;
CG_RESULT trades_callback(cg_conn_t*, cg_listener_t*, struct cg_msg_t* msg, void* p)
{
    switch (msg->type)
    {
    case CG_MSG_STREAM_DATA: 
        {
            if(likely(deals_online)) {
                if(msg->data_size == sizeof(deal)) {
                    deal* d = (deal*)msg->data;
                    fmap<int32_t, security>& tickers = *((fmap<int32_t, security>*)p);
                    auto it = tickers.find(d->isin_id);
                    if(it == tickers.end()) {
                        break;
                    }
                    security& s = it->second;
                    uint32_t direction = 0;
                    if(d->id_ord_buy > d->id_ord_sell)
                        direction = 1;
                    else if(d->id_ord_sell > d->id_ord_buy)
                        direction = 2;
                    price_t price = *d->price;
                    count_t count = {d->xamount * count_t::frac};
                    ttime_t etime = {d->moment_ns};
                    s.proceed_trade(parser::instance().t, direction, price, count, etime);
                }
                else if (msg->data_size == sizeof(heartbeat)) {
                    parser::instance().t.ping();
                    heartbeat* h = (heartbeat*)msg->data;
                    if(config::instance().log_plaza)
                        h->print_brief();
                    break;
                }
                else throw std::runtime_error(es() % "trades_callback unknown message, size: " % msg->data_size);
            }
            //else
            //    mlog() << "deals_callback stream_data skipped due to not online";
            break;
        }
    case CG_MSG_TN_BEGIN:
        {
            //mlog() << "deals_callback CG_MSG_TN_BEGIN";
            break;
        } 
    case CG_MSG_TN_COMMIT:
        {
            parser::instance().t.flush();
            //mlog() << "deals_callback CG_MSG_TN_COMMIT";
            break;
        } 
    case CG_MSG_P2REPL_CLEARDELETED:
        {
            if(config::instance().log_plaza)
                mlog() << "deals_callback CG_MSG_P2REPL_CLEARDELETED";
            break;
        } 
    case CG_MSG_OPEN:
        {
            if(config::instance().log_plaza)
                mlog() << "deals_callback cg_msg_open";
            break;
        } 
    case CG_MSG_CLOSE:
        {
            if(config::instance().log_plaza)
                mlog() << "deals_callback cg_msg_close";
            parser::instance().trades.set_closed();
            break;
        } 
    case CG_MSG_P2MQ_TIMEOUT: 
        {
            if(config::instance().log_plaza)
                mlog(mlog::critical) << "deals_callback plaza timeout";
            break;
        }
    case CG_MSG_P2REPL_ONLINE: 
        {
            deals_online = true;
            if(config::instance().log_plaza)
                mlog() << "deals_callback plaza online setted";
            break;
        }
    case CG_MSG_P2REPL_REPLSTATE: 
        {
            if(config::instance().log_plaza)
                mlog() << "deals_callback CG_MSG_P2REPL_REPLSTATE, data, str: " << str_holder((char*)msg->data);
            parser::instance().trades.set_replstate(msg);
            break;
        }
    default:
        if(config::instance().log_plaza)
            mlog(mlog::critical) << "deals_callback unknown message type: " << msg->type;
    }
    parser::instance().trades.set_call();
    return CG_ERR_OK;
}
