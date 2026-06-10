#include "test_framework.h"

int main(int argc, char **argv) {
    const std::string suite_filter = argc > 1 ? argv[1] : "";
    return ttest::run_all(suite_filter);
}
