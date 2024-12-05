#include <iostream>
#include <string>
#include <map>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <regex>
#include <signal.h>
#include <sys/types.h>
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include <json/json.h> // 假设使用 JSON 库，例如 jsoncpp 处理 JSON 数据
#include <sqlite3.h>   // 包含SQLite的头文件
#include <sstream>
#include <sys/wait.h>  // 添加这行
#include <unistd.h>     // 添加这行，如果没有的话

typedef websocketpp::server<websocketpp::config::asio> server;
using websocketpp::connection_hdl;

class MyServer {
public:
    MyServer() {
        // 初始化WebSocket服务端
        m_server.init_asio();
        m_server.set_reuse_addr(true);
        m_server.set_open_handler(std::bind(&MyServer::on_open, this, std::placeholders::_1));
        m_server.set_close_handler(std::bind(&MyServer::on_close, this, std::placeholders::_1));
        m_server.set_message_handler(std::bind(&MyServer::on_message, this, std::placeholders::_1, std::placeholders::_2));

        // 初始化数据库
        init_db();
    }

    ~MyServer() {
        // 清理所有RTSP进程
        for (const auto& entry : m_rtsp_pids) {
            kill(entry.second, SIGTERM);
        }
        sqlite3_close(m_db);  // 关闭数据库连接
    }

    void on_open(connection_hdl hdl) {
        std::cout << "Connection opened!" << std::endl;
    }

    void on_close(connection_hdl hdl) {
        std::cout << "Connection closed!" << std::endl;
    }

    void on_message(connection_hdl hdl, server::message_ptr msg) {
        std::string payload = msg->get_payload();
        if (payload.rfind("add_rtsp:", 0) == 0) {
            // 添加RTSP链接
            std::string url = payload.substr(9);
            add_rtsp_thread(url);
        } else if (payload.rfind("delete_rtsp:", 0) == 0) {
            // 删除RTSP线程
            std::string url = payload.substr(12);
            delete_rtsp_thread(url);
        } else if (payload == "list_rtsp") {
            // 查询当前RTSP线程信息
            std::string response = list_rtsp_threads();
            m_server.send(hdl, response, websocketpp::frame::opcode::text);
        } else if (payload.rfind("query_database:", 0) == 0) {
            // 查询数据库请求
            std::string query_params = payload.substr(15);
            auto result_json = query_database(query_params);
            m_server.send(hdl, result_json, websocketpp::frame::opcode::text);
        }
    }

    void run(uint16_t port) {
        try {
            m_server.listen(port);
            m_server.start_accept();
            std::cout << "Server listening on port " << port << std::endl;
            m_server.run();
        } catch (const std::exception &e) {
            std::cerr << "Error: " << e.what() << std::endl;
        }
    }

private:
    server m_server;
    std::map<std::string, pid_t> m_rtsp_pids;  // 保存IP到进程ID的映射
    std::mutex m_mutex;
    sqlite3* m_db; // SQLite数据库指针

    void init_db() {
        if (sqlite3_open("data.db", &m_db)) {
            std::cerr << "Can't open database: " << sqlite3_errmsg(m_db) << std::endl;
            return;
        }
    }

    void start_rtsp_process(const std::string& ip, const std::string& url) {
        pid_t pid = fork();
        if (pid == 0) {
            // 子进程执行的部分
            execlp("./aibox", "./aibox", url.c_str(), (char*)nullptr);
            std::cerr << "execlp failed: " << strerror(errno) << std::endl;
            std::exit(1);  // 如果 execlp 失败，退出子进程
        } else if (pid > 0) {
            // 父进程部分
            m_rtsp_pids[ip] = pid;
            std::cout << "Started aibox with PID " << pid << " for IP: " << ip << std::endl;

            // 启动一个新线程来等待子进程
            waitForChildProcess(pid);
        } else {
            std::cerr << "Failed to fork for IP: " << ip << std::endl;
        }
    }

    void waitForChildProcess(pid_t pid) {
        int status;
        waitpid(pid, &status, 0);  // 等待子进程退出

        if (WIFEXITED(status)) {
            std::cout << "Child process " << pid << " exited with status " << WEXITSTATUS(status) << std::endl;
        } else if (WIFSIGNALED(status)) {
            std::cout << "Child process " << pid << " was terminated by signal " << WTERMSIG(status) << std::endl;
        } else {
            std::cout << "Child process " << pid << " exited abnormally." << std::endl;
        }
    }

    void add_rtsp_thread(const std::string& url) {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string ip = extract_ip(url);
        if (m_rtsp_pids.find(ip) != m_rtsp_pids.end()) {
            std::cout << "RTSP thread already exists for IP: " << ip << std::endl;
            return;
        }

        std::thread rtspThread(std::bind(&MyServer::start_rtsp_process, this, ip, url));
        rtspThread.detach();  // 分离线程，使其在后台执行，不阻塞主线程
    }

    void delete_rtsp_thread(const std::string& url) {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string ip = extract_ip(url);
        auto it = m_rtsp_pids.find(ip);
        if (it != m_rtsp_pids.end()) {
            pid_t pid = it->second;
            if (kill(pid, SIGTERM) == 0) {
                std::cout << "Terminated aibox with PID " << pid << " for IP: " << ip << std::endl;
            } else {
                std::cerr << "Failed to terminate PID " << pid << " for IP: " << ip << std::endl;
            }
            m_rtsp_pids.erase(it);
        } else {
            std::cout << "No RTSP thread found for IP: " << ip << std::endl;
        }
    }

    std::string list_rtsp_threads() {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::ostringstream oss;
        oss << "Current RTSP threads:\n";
        for (const auto& entry : m_rtsp_pids) {
            oss << "IP: " << entry.first << ", PID: " << entry.second << "\n";
        }
        return oss.str();
    }

    std::string extract_ip(const std::string& rtsp_url) {
        std::regex ip_regex(R"((\d{1,3}\.){3}\d{1,3})"); // 匹配IPv4地址的正则表达式
        std::smatch match;
        if (std::regex_search(rtsp_url, match, ip_regex) && !match.empty()) {
            return match.str();
        }
        return "0.0.0.0"; // 默认值，如果没有找到IP
    }

    std::string query_database(const std::string& query_params) {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string rtsp_url, timestamp;
        std::istringstream iss(query_params);
        
        // 确保输入格式正确
        if (!std::getline(iss, rtsp_url, ',') || !std::getline(iss, timestamp)) {
            std::cerr << "Invalid query parameters: " << query_params << std::endl;
            return "{\"error\": \"Invalid query parameters.\"}";
        }

        // 记录获取的输入日志
        std::cout << "Received query parameters: rtsp_url = " << rtsp_url << ", timestamp = " << timestamp << std::endl;

        // 获取 IP 地址
        std::string ip_address = extract_ip(rtsp_url);
        std::cout << "IP Address: " << ip_address << ", Timestamp: " << timestamp << std::endl;


        // SQL 查询
        std::string sql = "SELECT timestamp, ip_address, rtsp_url, data FROM rtsp_logs WHERE ip_address = ? AND timestamp = ?;";
        sqlite3_stmt* stmt;
        Json::Value result;

        if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, ip_address.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, timestamp.c_str(), -1, SQLITE_STATIC);

            // 执行查询并记录结果日志
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                std::string db_timestamp = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                std::string db_ip_address = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                std::string db_rtsp_url = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                std::string db_data = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));

                Json::Value row;
                row["timestamp"] = db_timestamp;
                row["ip_address"] = db_ip_address;
                row["rtsp_url"] = db_rtsp_url;
                row["data"] = db_data;
                result.append(row);

                // 记录每行结果日志
                std::cout << "Query result: timestamp = " << db_timestamp
                        << ", ip_address = " << db_ip_address
                        << ", rtsp_url = " << db_rtsp_url
                        << ", data = " << db_data << std::endl;
            }

            int step_result = sqlite3_step(stmt);
            if (step_result != SQLITE_DONE) {
                std::cerr << "SQL error during step: " << sqlite3_errmsg(m_db) << std::endl;
            }

            sqlite3_finalize(stmt);
        } else {
            std::cerr << "SQL prepare error: " << sqlite3_errmsg(m_db) << std::endl;
        }

        Json::StreamWriterBuilder writer;
        std::string json_output = Json::writeString(writer, result);
        return json_output;
    }


};

int main() {
    MyServer server;
    server.run(8081);
    return 0;
}




//  g++ -std=c++11 -o websocket_server main.cpp -lboost_system -lpthread
//g++ -o websocket_server main.cpp -lboost_system -lpthread -lsqlite3 -I/usr/include/jsoncpp -L/usr/lib/aarch64-linux-gnu -ljsoncpp

