/*
  Create a TCP echo server, which echo you back what you sent.

  How to run:
  $ ./reactor/examples/echo_server 8080

  How to test (using netcat):
  $ nc 127.0.0.1 8080
*/
#include <elle/With.hh>
#include <elle/utility/Move.hh>
#include <elle/Exception.hh>

#include <elle/reactor/Scope.hh>
#include <elle/reactor/network/Error.hh>
#include <elle/reactor/network/TCPSocket.hh>
#include <elle/reactor/network/TCPServer.hh>
#include <elle/reactor/scheduler.hh>

struct Node {
  std::string host;
  std::string port;

  Node(std::string host, std::string port)
    : host{std::move(host)}, port{std::move(port)}
  {}

  auto connect() const
  {
    return std::make_unique<elle::reactor::network::TCPSocket>(host, port);
  }
};

class Nodes {
public:
  using Collection = std::vector<Node>;
  using Iter = Collection::const_iterator;

public:
  Nodes(std::initializer_list<Node> nodes)
    : nodes{nodes}
  {
    assert(not this->nodes.empty());
    iter = this->nodes.cend();
  }

  const Node& next()
  {
    // round robin
    const auto& node = [this]()->const Node&
    {
      if(iter != nodes.cend()) {
        return *iter;
      }
      // reached end, start from beginning
      else {
        iter = Iter{nodes.cbegin()};
        return *iter;
      }
    }();

    ++iter;

    return node;
  }

private:
  const std::vector<Node> nodes;
  Iter iter;
};

struct Connection {
  using Socket = elle::reactor::network::TCPSocket;
  using Socket_ptr = std::unique_ptr<Socket>;
  using Thread = elle::reactor::Thread;
  using Thread_ptr = std::unique_ptr<Thread>;
  using Collection = std::map<Socket::EndPoint, std::unique_ptr<Connection>>;

  Collection& collection;
  Socket_ptr outside;
  Socket_ptr inside;

  Thread_ptr out_to_in;
  Thread_ptr in_to_out;

  Connection(Collection& col, Socket_ptr out, Socket_ptr in)
    : collection{col},
      outside{std::move(out)},
      inside{std::move(in)}
  {
    out_to_in = std::make_unique<Thread>(
      elle::sprintf("conn %s", outside),
      [this] {
        try
        {
          while (true)
          {
            auto payload = outside->read_some(4096);
            inside->write(payload);
          }
        }
        catch (elle::reactor::network::ConnectionClosed const&)
        {
          std::cout << "Connection closed in client_out" << std::endl;
          throw;
        }
        //this->close();
      });

    in_to_out = std::make_unique<Thread>(
      elle::sprintf("conn %s", inside),
      [this] {
        try
        {
          while (true)
          {
            auto payload = inside->read_some(4096);
            outside->write(payload);
          }
        }
        catch (elle::reactor::network::ConnectionClosed const&)
        {
          std::cout << "Connection closed in client_in" << std::endl;
          throw;
        }
        //this->close();
      });
  }

  auto id() const
  {
    return outside->peer();
  }

  void close()
  {
    collection[id()] = nullptr;
  }
};

int
main(int argc, char* argv[])
{
  try
  {
    if (argc != 2)
    {
      std::cerr << "Usage: " << argv[0] << " <port>" << std::endl;
      return 1;
    }
    // Create a Scheduler, the coroutines operator.
    elle::reactor::Scheduler sched;
    // Properly terminate the scheduler in case of SIGINT.
    sched.signal_handle(SIGINT, [&sched] { sched.terminate(); });

    Nodes nodes{
      {"localhost", "8090"},
      {"localhost", "8091"}
    };

    // Create a coroutine (named elle::reactor::Thread).
    elle::reactor::Thread acceptor(sched, "acceptor", [&]
      {
        elle::reactor::network::TCPServer server;
        auto port = std::atoi(argv[1]);
        server.listen(port);
        // Scope enable to start tasks and make sure they are terminated upon
        // destruction, elle::With handles nested exceptions.
        elle::With<elle::reactor::Scope>() << [&] (elle::reactor::Scope& scope)
        {
          Connection::Collection connections;
          while (true)
          {
            try
            {
              // Server::accept yields until it gets a connection.
              auto outside = elle::utility::move_on_copy(server.accept());
              // Connect to one of our nodes
              auto inside = nodes.next().connect();

              auto conn = std::make_unique<Connection>(connections, std::move(outside), std::move(inside));
              connections.emplace(conn->id(), std::move(conn));
            }
            catch (elle::reactor::network::ConnectionClosed const&)
            {
              std::cout << "Connection closed" << std::endl;
            }
          } // < while(true)
          scope.wait();
        }; // < scope
      }); // < thread acceptor
    // Run the Scheduler until all coroutines are over or it gets interrupted
    // (by a signal or programmatically).
    sched.run();
  }
  catch (...)
  {
    std::cerr << "fatal error: " << elle::exception_string() << std::endl;
    return 1;
  }
  return 0;
}
