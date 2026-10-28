/*
 * SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <memory>
#include <vector>
#include <functional>
#include <string>
#include <thread>
#include <chrono>
#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>

namespace vsomeip {

enum class event_type_e : uint8_t {
    ET_EVENT = 0x00
};

enum class state_type_e : uint8_t {
    ST_REGISTERED = 0x00,
    ST_DEREGISTERED = 0x01
};

struct payload {
    virtual ~payload() = default;
    virtual void set_data(const uint8_t *data, uint32_t size) = 0;
    virtual const uint8_t* get_data() const = 0;
    virtual uint32_t get_length() const = 0;
};

struct message {
    virtual ~message() = default;
    virtual std::shared_ptr<payload> get_payload() const = 0;
    virtual uint32_t get_service() const = 0;
    virtual uint32_t get_instance() const = 0;
    virtual uint32_t get_method() const = 0;
};

class payload_impl : public payload {
    std::vector<uint8_t> data_;
public:
    void set_data(const uint8_t *data, uint32_t size) override {
        data_.assign(data, data + size);
    }
    const uint8_t* get_data() const override { return data_.data(); }
    uint32_t get_length() const override { return data_.size(); }
};

class message_impl : public message {
    std::shared_ptr<payload> payload_;
    uint32_t service_;
    uint32_t instance_;
    uint32_t method_;
public:
    message_impl(uint32_t s, uint32_t i, uint32_t m, std::shared_ptr<payload> p)
        : payload_(p), service_(s), instance_(i), method_(m) {}
    std::shared_ptr<payload> get_payload() const override { return payload_; }
    uint32_t get_service() const override { return service_; }
    uint32_t get_instance() const override { return instance_; }
    uint32_t get_method() const override { return method_; }
};

class application {
public:
    virtual ~application() = default;
    virtual const std::string& get_name() const = 0;
    virtual void init() = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void offer_service(uint32_t service, uint32_t instance) = 0;
    virtual void offer_event(uint32_t service, uint32_t instance, uint32_t event, 
                              const std::vector<uint32_t>& groups, event_type_e type) = 0;
    virtual void notify(uint32_t service, uint32_t instance, uint32_t event, std::shared_ptr<payload> payload) = 0;
    virtual void request_service(uint32_t service, uint32_t instance) = 0;
    virtual void subscribe(uint32_t service, uint32_t instance, uint32_t event) = 0;
    virtual void register_state_handler(std::function<void(state_type_e)> handler) = 0;
    virtual void register_message_handler(uint32_t service, uint32_t instance, uint32_t event, 
                                          std::function<void(const std::shared_ptr<message>&)> handler) = 0;
};

class runtime {
public:
    virtual ~runtime() = default;
    static std::shared_ptr<runtime> get();
    virtual std::shared_ptr<application> create_application(const std::string& name) = 0;
    virtual std::shared_ptr<payload> create_payload() = 0;
};

// UDP Socket-based mock implementation of application
class mock_application : public application {
    std::string name_;
    int sock_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread rx_thread_;
    std::function<void(state_type_e)> state_handler_;
    
    struct handler_key {
        uint32_t service;
        uint32_t instance;
        uint32_t event;
        bool operator==(const handler_key& o) const {
            return service == o.service && instance == o.instance && event == o.event;
        }
    };
    
    struct handler_entry {
        handler_key key;
        std::function<void(const std::shared_ptr<message>&)> callback;
    };
    
    std::vector<handler_entry> handlers_;

public:
    mock_application(const std::string& name) : name_(name) {}
    
    ~mock_application() override {
        running_ = false;
        cleanup();
    }
    
    const std::string& get_name() const override { return name_; }
    
    void init() override {
        std::cout << "[vsomeip-mock] Initializing application \"" << name_ << "\"" << std::endl;
    }
    
    void start() override {
        if (running_) return;
        running_ = true;
        
        // If we have registered message handlers, we need to bind and receive UDP packets
        if (!handlers_.empty()) {
            sock_fd_ = socket(AF_INET, SOCK_DGRAM, 0);
            if (sock_fd_ < 0) {
                std::cerr << "[vsomeip-mock] Failed to create socket" << std::endl;
                return;
            }
            
            // Allow address reuse
            int opt = 1;
            setsockopt(sock_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            
            // close() does not wake a recv() blocked in another thread; a timeout lets rx_loop see running_ == false
            timeval tv{0, 100000};
            setsockopt(sock_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(30490); // Default SOME/IP mock port
            addr.sin_addr.s_addr = INADDR_ANY;
            
            if (bind(sock_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                std::cerr << "[vsomeip-mock] Bind failed on port 30490. Starting in client-only mode." << std::endl;
                close(sock_fd_);
                sock_fd_ = -1;
            } else {
                std::cout << "[vsomeip-mock] Listening for SOME/IP packets on port 30490" << std::endl;
                rx_thread_ = std::thread([this]() { this->rx_loop(); });
            }
        }
        
        if (state_handler_) {
            state_handler_(state_type_e::ST_REGISTERED);
        }

        // Block main thread until stop() is called, simulating COVESA vsomeip behavior
        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        cleanup();
        if (state_handler_) {
            state_handler_(state_type_e::ST_DEREGISTERED);
        }
    }
    
    // Async-signal-safe: only flags shutdown; start() performs the actual teardown.
    void stop() override {
        running_ = false;
    }
    
    void offer_service(uint32_t service, uint32_t instance) override {
        std::cout << "[vsomeip-mock] Offering service 0x" << std::hex << service 
                  << " instance 0x" << instance << std::dec << std::endl;
    }
    
    void offer_event(uint32_t service, uint32_t instance, uint32_t event, 
                      const std::vector<uint32_t>& groups, event_type_e type) override {
        std::cout << "[vsomeip-mock] Offering event 0x" << std::hex << event 
                  << " on service 0x" << service << std::dec << std::endl;
    }
    
    void notify(uint32_t service, uint32_t instance, uint32_t event, std::shared_ptr<payload> payload) override {
        // Send via UDP loopback
        int send_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (send_fd < 0) return;
        
        sockaddr_in dest{};
        dest.sin_family = AF_INET;
        dest.sin_port = htons(30490);
        dest.sin_addr.s_addr = inet_addr("127.0.0.1");
        
        // Serialize packet
        std::vector<uint8_t> buffer(16 + payload->get_length());
        uint32_t s = htonl(service);
        uint32_t i = htonl(instance);
        uint32_t e = htonl(event);
        uint32_t len = htonl(payload->get_length());
        
        std::memcpy(&buffer[0], &s, 4);
        std::memcpy(&buffer[4], &i, 4);
        std::memcpy(&buffer[8], &e, 4);
        std::memcpy(&buffer[12], &len, 4);
        std::memcpy(&buffer[16], payload->get_data(), payload->get_length());
        
        sendto(send_fd, buffer.data(), buffer.size(), 0, (struct sockaddr*)&dest, sizeof(dest));
        close(send_fd);
    }
    
    void request_service(uint32_t service, uint32_t instance) override {
        std::cout << "[vsomeip-mock] Requesting service 0x" << std::hex << service 
                  << " instance 0x" << instance << std::dec << std::endl;
    }
    
    void subscribe(uint32_t service, uint32_t instance, uint32_t event) override {
        std::cout << "[vsomeip-mock] Subscribed to event 0x" << std::hex << event 
                  << " on service 0x" << service << std::dec << std::endl;
    }
    
    void register_state_handler(std::function<void(state_type_e)> handler) override {
        state_handler_ = handler;
    }
    
    void register_message_handler(uint32_t service, uint32_t instance, uint32_t event, 
                                  std::function<void(const std::shared_ptr<message>&)> handler) override {
        handlers_.push_back({{service, instance, event}, handler});
    }

private:
    void cleanup() {
        if (rx_thread_.joinable()) {
            rx_thread_.join();
        }
        if (sock_fd_ != -1) {
            close(sock_fd_);
            sock_fd_ = -1;
        }
    }

    void rx_loop() {
        std::vector<uint8_t> rx_buffer(65536);
        while (running_) {
            ssize_t n = recv(sock_fd_, rx_buffer.data(), rx_buffer.size(), 0);
            if (n < 16) {
                if (n < 0 && running_ && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                continue;
            }
            
            uint32_t service, instance, event, len;
            std::memcpy(&service, &rx_buffer[0], 4);
            std::memcpy(&instance, &rx_buffer[4], 4);
            std::memcpy(&event, &rx_buffer[8], 4);
            std::memcpy(&len, &rx_buffer[12], 4);
            
            service = ntohl(service);
            instance = ntohl(instance);
            event = ntohl(event);
            len = ntohl(len);
            
            if (static_cast<size_t>(n) < 16 + len) continue;
            
            auto p = std::make_shared<payload_impl>();
            p->set_data(&rx_buffer[16], len);
            
            auto msg = std::make_shared<message_impl>(service, instance, event, p);
            
            // Dispatch to registered handlers
            for (const auto& entry : handlers_) {
                if (entry.key.service == service && 
                    entry.key.instance == instance && 
                    entry.key.event == event) {
                    entry.callback(msg);
                }
            }
        }
    }
};

class mock_runtime : public runtime {
public:
    std::shared_ptr<application> create_application(const std::string& name) override {
        return std::make_shared<mock_application>(name);
    }
    
    std::shared_ptr<payload> create_payload() override {
        return std::make_shared<payload_impl>();
    }
};

inline std::shared_ptr<runtime> runtime::get() {
    static auto rt = std::make_shared<mock_runtime>();
    return rt;
}

} // namespace vsomeip

