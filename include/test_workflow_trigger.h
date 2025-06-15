// Dummy C++ header file to test CI workflow triggers
// This file should trigger the cpp-build job when modified in a PR

#pragma once

#include <vector>
#include <string>

namespace CommunityShaders {
    namespace Test {

        struct WorkflowTestConfig {
            std::string name;
            int priority;
            bool enabled;

            WorkflowTestConfig(const std::string& n, int p, bool e)
                : name(n), priority(p), enabled(e) {}
        };

        class WorkflowTestManager {
        private:
            std::vector<WorkflowTestConfig> configs;

        public:
            void addConfig(const WorkflowTestConfig& config);
            void removeConfig(const std::string& name);
            bool hasConfig(const std::string& name) const;

            size_t getConfigCount() const { return configs.size(); }

            // Template function to test modern C++ features
            template<typename T>
            void processValue(T&& value) {
                // Process the value
                static_cast<void>(value);
            }
        };

        // Function declarations
        void initializeWorkflowTest();
        void cleanupWorkflowTest();

        // Constants for testing
        constexpr int MAX_TEST_CONFIGS = 100;
        constexpr const char* TEST_VERSION = "1.0.0";
    }
}

// This file exists to test:
// - C++ header file change detection in .github/workflows/build.yaml
// - cpp-build job triggering
// - File patterns: **.h, **.hpp, include/**
