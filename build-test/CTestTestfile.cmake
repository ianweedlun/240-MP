# CMake generated Testfile for 
# Source directory: /home/ian/src/240-MP
# Build directory: /home/ian/src/240-MP/build-test
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[retroarch-backend]=] "/home/ian/src/240-MP/build-test/retroarch-tests")
set_tests_properties([=[retroarch-backend]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/ian/src/240-MP/CMakeLists.txt;213;add_test;/home/ian/src/240-MP/CMakeLists.txt;0;")
add_test([=[weather-codes]=] "/home/ian/src/240-MP/build-test/weather-tests")
set_tests_properties([=[weather-codes]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/ian/src/240-MP/CMakeLists.txt;225;add_test;/home/ian/src/240-MP/CMakeLists.txt;0;")
add_test([=[mpv-basic-options]=] "/home/ian/src/240-MP/build-test/mpv-basic-options-tests")
set_tests_properties([=[mpv-basic-options]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/ian/src/240-MP/CMakeLists.txt;260;add_test;/home/ian/src/240-MP/CMakeLists.txt;0;")
