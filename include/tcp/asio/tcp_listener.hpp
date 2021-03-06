//***************************************************************
//	created:	2020/08/01
//	author:		wkui
//***************************************************************

#pragma once
#include <tuple>
#include <memory>

#include "tcp_connection.hpp"

namespace knet
{
	namespace tcp
	{
		using asio::ip::tcp;
		template <class T, class F = ConnectionFactory<T>, class Worker = EventWorker, class... Args>
		class TcpListener final
		{
		public:
			using TPtr = std::shared_ptr<T>;
			using Factory = F;
			using FactoryPtr = F *;
			using WorkerPtr = std::shared_ptr<Worker>;
			using SocketPtr = std::shared_ptr<typename T::ConnSock>;

			// TcpListener(FactoryPtr fac, uint32_t num, WorkerPtr lisWorker = std::make_shared<Worker>())
			// 	: listen_worker(lisWorker)
			// {
			// 	m.factory = fac;
			// 	if (!listen_worker)
			// 	{
			// 		elog("can't live without listen worker, fail to start ");
			// 		return;
			// 	}
			// 	listen_worker->start();

			// 	tcp_acceptor = std::make_shared<asio::ip::tcp::acceptor>(lisWorker->context());
			// 	for (uint32_t i = 0; i < num; i++)
			// 	{
			// 		m.user_workers.push_back(std::make_shared<Worker>());
			// 	}
			// }

			// TcpListener(FactoryPtr fac, std::vector<WorkerPtr> workers,
			// 			WorkerPtr lisWorker = std::make_shared<Worker>())
			// 	: listen_worker(lisWorker)
			// {

			// 	m.factory = fac;
			// 	if (!listen_worker)
			// 	{
			// 		elog("can't live without listen worker");
			// 	}
			// 	listen_worker->start();
			// 	dlog("start listener in one worker");
			// 	tcp_acceptor = std::make_shared<asio::ip::tcp::acceptor>(lisWorker->context());
			// 	if (!workers.empty())
			// 	{
			// 		for (auto worker : workers)
			// 		{
			// 			m.user_workers.push_back(worker);
			// 		}
			// 	}
			// }

			TcpListener(
				FactoryPtr fac = nullptr, WorkerPtr lisWorker = std::make_shared<Worker>(), Args... args)
				: listen_worker(lisWorker), conn_args(args...)
			{
				m.factory = fac;
				if (listen_worker)
				{
					listen_worker->start();
					tcp_acceptor = std::make_shared<asio::ip::tcp::acceptor>(listen_worker->context());
				}
				else
				{
					elog("can't live without listen worker");
				}
			}

			TcpListener(WorkerPtr lisWorker, Args... args)
				: listen_worker(lisWorker), conn_args(args...)
			{
				dlog("create listener without factory");
				m.factory = nullptr;

				if (listen_worker)
				{
					listen_worker->start();
					tcp_acceptor = std::make_shared<asio::ip::tcp::acceptor>(listen_worker->context());
				}
				else
				{
					elog("can't live without listen worker");
				}
			}

			void add_worker(WorkerPtr worker)
			{
				if (worker)
				{
					m.user_workers.push_back(worker);
				}
			}

			TcpListener(const TcpListener &) = delete;

			~TcpListener() {}

			bool start(NetOptions opt, void *sslCtx = nullptr)
			{
				m.options = opt;
				m.ssl_context = sslCtx;
				if (!m.is_running)
				{
					m.is_running = true;

					asio::ip::tcp::endpoint endpoint(asio::ip::make_address(opt.host), opt.port);

					// this->tcp_acceptor.open(asio::ip::tcp::v4());
					this->tcp_acceptor->open(endpoint.protocol());
					if (tcp_acceptor->is_open())
					{
						this->tcp_acceptor->set_option(asio::socket_base::reuse_address(true));
						//	this->tcp_acceptor.set_option(asio::ip::tcp::no_delay(true));
						this->tcp_acceptor->non_blocking(true);

						asio::socket_base::send_buffer_size SNDBUF(m.options.send_buffer_size);
						this->tcp_acceptor->set_option(SNDBUF);
						asio::socket_base::receive_buffer_size RCVBUF(m.options.recv_buffer_size);
						this->tcp_acceptor->set_option(RCVBUF);

						asio::error_code ec;
						this->tcp_acceptor->bind(endpoint, ec);
						if (ec)
						{
							elog("bind address failed {}:{}", opt.host, opt.port);
							m.is_running = false;
							return false;
						}
						this->tcp_acceptor->listen(m.options.backlogs, ec);

						if (ec)
						{
							elog("start listen failed");
							m.is_running = false;
							return false;
						}
						this->do_accept();
					}
					else
					{
						return false;
					}
				}
				return true;
			}

			bool start(uint32_t port = 9999, const std::string &host = "0.0.0.0", void *ssl = nullptr)
			{
				m.options.host = host;
				m.options.port = port;
				return start(m.options, ssl);
			}

			void stop()
			{
				dlog("stop listener thread");
				if (m.is_running)
				{
					m.is_running = false;
					tcp_acceptor->close();
				}
			}

			void destroy(std::shared_ptr<T> conn)
			{
				asio::post(listen_worker->context(), [this, conn]() {
					if (m.factory)
					{
						m.factory->destroy(conn);
					}
				});
			}

		private:
			void do_accept()
			{
				// dlog("accept new connection ");
				auto worker = this->get_worker();
				if (!worker)
				{
					elog("no event worker, can't start");
					return;
				}

				auto socket = std::make_shared<typename T::ConnSock>(
					worker->thread_id(), worker->context(), m.ssl_context);
				tcp_acceptor->async_accept(socket->socket(), [this, socket, worker](std::error_code ec) {
					if (!ec)
					{
						dlog("accept new connection ");
						this->init_conn(worker, socket);
						do_accept();
					}
					else
					{
						elog("accept error");
					}
				});
			}

			WorkerPtr get_worker()
			{
				if (!m.user_workers.empty())
				{
					dlog("dispatch to worker {}", m.worker_index);
					return m.user_workers[m.worker_index++ % m.user_workers.size()];
				}
				else
				{
					dlog("dispatch work  to listen worker {}", std::this_thread::get_id());
					return listen_worker;
				}
			}

			void init_conn(WorkerPtr worker, std::shared_ptr<typename T::ConnSock> socket)
			{

				if (worker)
				{
					asio::dispatch(worker->context(), [=]() {
						auto conn = create_connection(socket, worker); 
					 
						conn->destroyer = std::bind(
							&TcpListener<T, F, Worker, Args...>::destroy, this, std::placeholders::_1);

						conn->handle_event(EVT_CONNECT);
						socket->do_read();
					});
				}
			}

			static TPtr factory_create_helper(FactoryPtr fac, Args... args)
			{
				if (fac)
				{
					wlog("create connection by factory");
					return fac->create(std::forward<Args>(args)... );
				}
				else
				{
					wlog("create connection by default");
					return std::make_shared<T>( std::forward<Args>(args)... );
				}
			}

			TPtr create_connection(SocketPtr sock ,WorkerPtr worker)
			{

				auto conn = std::apply(&TcpListener<T, F, Worker, Args...>::factory_create_helper,
									   std::tuple_cat(std::make_tuple(m.factory), conn_args));

				conn->init(m.factory, sock, worker);
				return conn;
			}

			struct
			{
				uint32_t worker_index = 0;
				std::vector<WorkerPtr> user_workers;
				NetOptions options;
				bool is_running = false;
				void *ssl_context = nullptr;
				FactoryPtr factory = nullptr;
			} m;

			std::shared_ptr<asio::ip::tcp::acceptor> tcp_acceptor;
			WorkerPtr listen_worker;
			std::tuple<Args...> conn_args;
		};

		template <typename T, typename ... Args >
		    using DefaultTcpListener = TcpListener<T, ConnectionFactory<T>, EventWorker, Args...>;

	} // namespace tcp

} // namespace knet
