cmake_minimum_required(VERSION 3.8)
project(distributed_election)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# find dependencies
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(rclcpp_lifecycle REQUIRED)
find_package(std_msgs REQUIRED)
find_package(lifecycle_msgs REQUIRED)

include_directories(include)

add_executable(simulation_orchestrator
  src/simulation_orchestrator.cpp
  src/simple_agent.cpp
)
ament_target_dependencies(simulation_orchestrator
  rclcpp
  rclcpp_lifecycle
  std_msgs
  lifecycle_msgs
)

add_executable(chaos_monkey
  src/chaos_monkey.cpp
)
ament_target_dependencies(chaos_monkey
  rclcpp
  rclcpp_lifecycle
  std_msgs
  lifecycle_msgs
)

install(TARGETS
  simulation_orchestrator
  chaos_monkey
  DESTINATION lib/${PROJECT_NAME}
)

install(DIRECTORY
  launch
  config
  DESTINATION share/${PROJECT_NAME}
)

if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  # the following line skips the linter which checks for copyrights
  # comment the line when a copyright and license is added to all source files
  set(ament_cmake_copyright_FOUND TRUE)
  # the following line skips cpplint (only works in a git repo)
  # comment the line when this package is in a git repo and when
  # a copyright and license is added to all source files
  set(ament_cmake_cpplint_FOUND TRUE)
  ament_lint_auto_find_test_dependencies()
endif()

ament_package()
