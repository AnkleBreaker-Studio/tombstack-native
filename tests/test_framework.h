#ifndef TOMBSTONE_TESTS_TEST_FRAMEWORK_H
#define TOMBSTONE_TESTS_TEST_FRAMEWORK_H

// Tiny dependency-free test runner: TEST_CASE registers into a global list,
// CHECK/CHECK_EQ throw on failure, run_all executes (optionally filtered by
// suite name, which is how CTest invokes one suite per test entry).

#include <cstdio>
#include <filesystem>
#include <functional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace ttest {

struct TestCase {
    std::string suite;
    std::string name;
    std::function<void()> body;
};

inline std::vector<TestCase> &registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(std::string suite, std::string name, std::function<void()> body) {
        registry().push_back(TestCase{std::move(suite), std::move(name), std::move(body)});
    }
};

struct Failure {
    std::string message;
};

template <typename T>
std::string display(const T &value) {
    std::ostringstream out;
    out << value;
    return out.str();
}

inline std::string display(bool value) { return value ? "true" : "false"; }

inline int run_all(const std::string &suite_filter) {
    int executed = 0;
    int failed = 0;
    for (const TestCase &test : registry()) {
        if (!suite_filter.empty() && test.suite != suite_filter) {
            continue;
        }
        ++executed;
        try {
            test.body();
            std::printf("[ OK ] %s.%s\n", test.suite.c_str(), test.name.c_str());
        } catch (const Failure &failure) {
            ++failed;
            std::printf("[FAIL] %s.%s\n       %s\n", test.suite.c_str(), test.name.c_str(),
                        failure.message.c_str());
        } catch (const std::exception &e) {
            ++failed;
            std::printf("[FAIL] %s.%s\n       unexpected exception: %s\n", test.suite.c_str(),
                        test.name.c_str(), e.what());
        }
    }
    if (executed == 0) {
        std::printf("no tests matched suite '%s'\n", suite_filter.c_str());
        return 1;
    }
    std::printf("%d test(s), %d failure(s)\n", executed, failed);
    return failed == 0 ? 0 : 1;
}

/** Self-deleting scratch directory for filesystem tests. */
class TempDir {
public:
    TempDir() {
        std::mt19937_64 engine{std::random_device{}()};
        path_ = std::filesystem::temp_directory_path() /
                ("tombstone-test-" + std::to_string(engine()));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;
    const std::filesystem::path &path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace ttest

#define TT_CONCAT2(a, b) a##b
#define TT_CONCAT(a, b) TT_CONCAT2(a, b)

#define TEST_CASE(suite, name)                                                       \
    static void TT_CONCAT(tt_body_, __LINE__)();                                     \
    static const ::ttest::Registrar TT_CONCAT(tt_reg_, __LINE__){                    \
        suite, name, &TT_CONCAT(tt_body_, __LINE__)};                                \
    static void TT_CONCAT(tt_body_, __LINE__)()

#define CHECK(condition)                                                             \
    do {                                                                             \
        if (!(condition)) {                                                          \
            throw ::ttest::Failure{std::string{"CHECK failed: "} + #condition +      \
                                   " (" + __FILE__ + ":" + std::to_string(__LINE__) + \
                                   ")"};                                             \
        }                                                                            \
    } while (false)

#define CHECK_EQ(actual, expected)                                                   \
    do {                                                                             \
        const auto tt_actual = (actual);                                             \
        const auto tt_expected = (expected);                                         \
        if (!(tt_actual == tt_expected)) {                                           \
            throw ::ttest::Failure{std::string{"CHECK_EQ failed (" } + __FILE__ +    \
                                   ":" + std::to_string(__LINE__) +                  \
                                   ")\n  actual:   " + ::ttest::display(tt_actual) + \
                                   "\n  expected: " + ::ttest::display(tt_expected)}; \
        }                                                                            \
    } while (false)

#endif  // TOMBSTONE_TESTS_TEST_FRAMEWORK_H
