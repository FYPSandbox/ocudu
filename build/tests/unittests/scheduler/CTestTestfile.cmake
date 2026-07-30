# CMake generated Testfile for 
# Source directory: /home/fyp5g/fyp/ocudu/tests/unittests/scheduler
# Build directory: /home/fyp5g/fyp/ocudu/build/tests/unittests/scheduler
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(pdcch_resource_allocator_test "pdcch_resource_allocator_test")
set_tests_properties(pdcch_resource_allocator_test PROPERTIES  _BACKTRACE_TRIPLES "/home/fyp5g/fyp/ocudu/tests/unittests/scheduler/CMakeLists.txt;31;add_test;/home/fyp5g/fyp/ocudu/tests/unittests/scheduler/CMakeLists.txt;0;")
add_test(scheduler_test "scheduler_test")
set_tests_properties(scheduler_test PROPERTIES  _BACKTRACE_TRIPLES "/home/fyp5g/fyp/ocudu/tests/unittests/scheduler/CMakeLists.txt;55;add_test;/home/fyp5g/fyp/ocudu/tests/unittests/scheduler/CMakeLists.txt;0;")
add_test(scheduler_metrics_handler_test "scheduler_metrics_handler_test")
set_tests_properties(scheduler_metrics_handler_test PROPERTIES  _BACKTRACE_TRIPLES "/home/fyp5g/fyp/ocudu/tests/unittests/scheduler/CMakeLists.txt;65;add_test;/home/fyp5g/fyp/ocudu/tests/unittests/scheduler/CMakeLists.txt;0;")
subdirs("test_utils")
subdirs("support")
subdirs("cell")
subdirs("common_scheduling")
subdirs("ue_context")
subdirs("ue_scheduling")
subdirs("uci_and_pucch")
subdirs("policy")
subdirs("config")
subdirs("slicing")
subdirs("srs_scheduling")
subdirs("rrm")
set_directory_properties(PROPERTIES LABELS "sched")
