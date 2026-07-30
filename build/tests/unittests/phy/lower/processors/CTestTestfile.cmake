# CMake generated Testfile for 
# Source directory: /home/fyp5g/fyp/ocudu/tests/unittests/phy/lower/processors
# Build directory: /home/fyp5g/fyp/ocudu/build/tests/unittests/phy/lower/processors
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(resource_request_pool_test "resource_request_pool_test")
set_tests_properties(resource_request_pool_test PROPERTIES  _BACKTRACE_TRIPLES "/home/fyp5g/fyp/ocudu/tests/unittests/phy/lower/processors/CMakeLists.txt;12;add_test;/home/fyp5g/fyp/ocudu/tests/unittests/phy/lower/processors/CMakeLists.txt;0;")
subdirs("downlink")
subdirs("uplink")
set_directory_properties(PROPERTIES LABELS "phy")
