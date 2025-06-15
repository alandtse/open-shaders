// Dummy C++ source file to test CI workflow triggers
// This file should trigger the cpp-build job when modified in a PR

#include <iostream>

namespace CommunityShaders
{
	namespace Test
	{

		class WorkflowTestClass
		{
		public:
			void testMethod()
			{
				std::cout << "This is a test C++ file for CI workflow validation" << std::endl;
			}

			int calculateValue(int input)
			{
				return input * 2 + 42;
			}
		};

		void globalTestFunction()
		{
			WorkflowTestClass test;
			test.testMethod();

			int result = test.calculateValue(10);
			std::cout << "Test result: " << result << std::endl;
		}
	}
}

// This file exists to test:
// - C++ file change detection in .github/workflows/build.yaml
// - cpp-build job triggering
// - File patterns: **.cpp, src/**
