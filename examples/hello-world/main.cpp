#include <chrono>
#include <condition_variable>
#include <cppboot/cppboot.hpp>
#include <mutex>
#include <stop_token>

namespace {

/// Logs a heartbeat from a managed worker until the application shuts down.
class HeartbeatModule : public cppboot::Module {
public:
    std::string_view name() const noexcept override { return "heartbeat"; }

    cppboot::Result<void> start(cppboot::ApplicationContext& context) override {
        const auto interval = context.config().getInt("hello.interval_seconds", 1);
        if (!interval) {
            return interval.error();
        }

        return context.workers().spawn(
            "heartbeat",
            [&logger = context.logger(), seconds = interval.value()](std::stop_token token) {
                std::mutex mutex;
                std::condition_variable_any wake;
                std::unique_lock lock{mutex};

                while (!token.stop_requested()) {
                    logger.info("Hello from CppBoot. Press Ctrl+C to stop.");
                    static_cast<void>(wake.wait_for(lock, token, std::chrono::seconds{seconds},
                                                    [] { return false; }));
                }
            });
    }
};

}  // namespace

int main(int argc, char* argv[]) {
    cppboot::Application app(argc, argv);
    app.emplaceModule<HeartbeatModule>();
    return app.run();
}
