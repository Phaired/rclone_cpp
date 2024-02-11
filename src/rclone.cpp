#include "rclone.hpp"
#include <iostream>
#include <regex>


namespace Iridium
{
    namespace ba = boost::asio;
    namespace bp = boost::process;
    namespace bs2 = boost::signals2;
    std::string rclone::_path_rclone;
    bool rclone::_is_initialized = false;
    const std::string rclone::endl = "\n";


    void rclone::initialize(const std::string &path_rclone)
    {
        _path_rclone = path_rclone;
        _is_initialized = true;
    }

    rclone::rclone()
    {
        if (!_is_initialized)
            throw std::runtime_error("rclone not initialized");
        _signal_every_line = std::make_unique<bs2::signal<void(const std::string &)>>();
        _signal_finish = std::make_unique<bs2::signal<void(int)>>();
    }

    rclone &rclone::wait_for_start()
    {
        std::unique_lock<std::mutex> lock(_mutex);
        _cv.wait(lock, [this]
        { return _state == state::launched; });
        return *this;
    }

    rclone &rclone::wait_for_finish()
    {
        std::unique_lock<std::mutex> lock(_mutex);
        _cv.wait(lock, [this]
        { return _state == state::finished; });
        return *this;
    }

    int rclone::exit_code() const
    {
        return _child.exit_code();
    }

    void rclone::write_input(const std::string &input)
    {
        if (_state != state::launched)
            throw std::runtime_error("rclone not started");

        if (_in->pipe().is_open())
            _in->pipe().write(input.c_str(), input.size());
    }

    void rclone::read_output()
    {
        std::string line;
        while (std::getline(*_out, line))
        {
            _output.emplace_back(line);
            if (_signal_every_line != nullptr)
                (*_signal_every_line)(line);
        }
        _out.reset();
    }

    void rclone::read_error()
    {
        std::string line;
        while (std::getline(*_err, line))
        {
            _error.emplace_back(line);
            std::cerr << line << std::endl;
        }
        _err.reset();
    }

    rclone &rclone::execute()
    {
        if (_state != state::not_launched)
            throw std::runtime_error("rclone already started");

        try
        {
            _in = std::make_unique<bp::opstream>();
            _out = std::make_unique<bp::ipstream>();
            _err = std::make_unique<bp::ipstream>();
            _child = bp::child(
                    _path_rclone, bp::args(_args),
                    bp::std_in < *_in, bp::std_out > *_out,
                    bp::std_err > *_err);
        } catch (const std::exception &e)
        {
            std::cerr << e.what() << std::endl;
            exit(1);
        }

        _state = state::launched;
        _cv.notify_all();

        ba::post(_pool, [this]
        {
            read_output();
            _counter++;
        });
        ba::post(_pool, [this]
        {
            read_error();
            _counter++;
        });

        ba::post(_pool, [this]
        {
            _child.wait();
            while (_counter < 2)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            _state = state::finished;
            if (_signal_finish != nullptr)
                (*_signal_finish)(_child.exit_code());
            _in.reset();
            _cv.notify_all();

        });

        return *this;

    }

    void rclone::stop()
    {
        _in.reset();
        _pool.stop();
        _child.terminate();
        _state = state::stopped;
    }

    rclone &rclone::operator<<(const std::string &input)
    {
        write_input(input);
        return *this;
    }


    rclone::~rclone()
    {
        if (_state == state::launched)
        {
//            if the child waiting for an input, close it
            _in->pipe().close();
            std::cerr << "rclone are destroyed without being stopped" << std::endl;
        }

    }

    rclone &rclone::every_line(const std::function<void(const std::string &)> &&callback)
    {

        _signal_every_line->connect(
                [this, callback](const std::string &line)
                {
                    ba::post(_pool, [callback, line]
                    {
                        callback(line);
                    });
                }
        );
        return *this;
    }

    rclone &rclone::finished(const std::function<void(int)> &&callback)
    {
        _signal_finish->connect(
                [this, callback](const int &exit_code)
                {
                    ba::post(_pool, [callback, exit_code]
                    {
                        callback(exit_code);
                    });
                }
        );
        return *this;
    }

    rclone &rclone::version()
    {
        _args = {"version"};
        return *this;
    }

    rclone &rclone::list_remotes(std::vector<rclone_remote> &remotes)
    {
        _args = {"listremotes", "--long"};
        _signal_finish->connect(
                [this, &remotes](const int &exit_code)
                {
                    ba::post(_pool, [this, &remotes, exit_code]
                    {
//                                example of line : "drive:  drive"
                        std::regex re(R"((\w+):\s+(\w+))");
                        std::smatch match;
                        for (const auto &line: _output)
                        {
                            if (std::regex_search(line, match, re))
                            {
                                remotes.emplace_back(match[1], rclone_remote::string_to_remote_type.at(match[2]), "");
                            }
                        }
                    });
                }
        );
        return *this;
    }

    rclone &rclone::config()
    {
        _args = {"config", "show"};
        return *this;
    }

    rclone &rclone::lsjson(const rclone_remote &remote)
    {
        _args = {"lsjson", remote.full_path()};
        return *this;
    }


} // namespace Iridium