# Install script for directory: /home/fyp5g/fyp/ocudu/configs

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Install shared libraries without execute permission?
if(NOT DEFINED CMAKE_INSTALL_SO_NO_EXE)
  set(CMAKE_INSTALL_SO_NO_EXE "1")
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set default install directory permissions.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/objdump")
endif()

if("x${CMAKE_INSTALL_COMPONENT}x" STREQUAL "xUnspecifiedx" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/share/ocudu" TYPE FILE FILES
    "/home/fyp5g/fyp/ocudu/configs/amf.yml"
    "/home/fyp5g/fyp/ocudu/configs/amf_sctp_multihoming.yml"
    "/home/fyp5g/fyp/ocudu/configs/cell_cfg_low_band_coverage_layer.yml"
    "/home/fyp5g/fyp/ocudu/configs/cell_cfg_max_128_ues.yml"
    "/home/fyp5g/fyp/ocudu/configs/cell_cfg_max_256_ues.yml"
    "/home/fyp5g/fyp/ocudu/configs/cell_cfg_max_32_ues.yml"
    "/home/fyp5g/fyp/ocudu/configs/cell_cfg_max_512_ues.yml"
    "/home/fyp5g/fyp/ocudu/configs/cell_cfg_max_64_ues.yml"
    "/home/fyp5g/fyp/ocudu/configs/cell_cfg_mid_band_capacity_layer.yml"
    "/home/fyp5g/fyp/ocudu/configs/cell_cfg_pucch_narrow_bw.yml"
    "/home/fyp5g/fyp/ocudu/configs/cu.yml"
    "/home/fyp5g/fyp/ocudu/configs/cu_cp.yml"
    "/home/fyp5g/fyp/ocudu/configs/cu_up.yml"
    "/home/fyp5g/fyp/ocudu/configs/cu_up_f1u_multiple_sockets.yml"
    "/home/fyp5g/fyp/ocudu/configs/cu_up_multiple_cps.yml"
    "/home/fyp5g/fyp/ocudu/configs/debug.yml"
    "/home/fyp5g/fyp/ocudu/configs/du_f1u_multiple_sockets.yml"
    "/home/fyp5g/fyp/ocudu/configs/du_rf_b200_tdd_n78_20mhz.yml"
    "/home/fyp5g/fyp/ocudu/configs/geo_coordinates.yml"
    "/home/fyp5g/fyp/ocudu/configs/geo_ntn.yml"
    "/home/fyp5g/fyp/ocudu/configs/gnb_custom_cell_properties.yml"
    "/home/fyp5g/fyp/ocudu/configs/gnb_rf_b200_tdd_n78_20mhz.yml"
    "/home/fyp5g/fyp/ocudu/configs/gnb_rf_b210_fdd_srsUE.yml"
    "/home/fyp5g/fyp/ocudu/configs/gnb_rf_n310_fdd_n3_20mhz.yml"
    "/home/fyp5g/fyp/ocudu/configs/gnb_ru_picocom_scb_tdd_n78_20mhz.yml"
    "/home/fyp5g/fyp/ocudu/configs/gnb_ru_ran550_tdd_n78_100mhz_4x2.yml"
    "/home/fyp5g/fyp/ocudu/configs/gnb_ru_rpqn4800e_tdd_n78_20mhz_2x2.yml"
    "/home/fyp5g/fyp/ocudu/configs/low_latency.yml"
    "/home/fyp5g/fyp/ocudu/configs/mimo_usrp.yml"
    "/home/fyp5g/fyp/ocudu/configs/mobility.yml"
    "/home/fyp5g/fyp/ocudu/configs/ngu_multiple_sockets.yml"
    "/home/fyp5g/fyp/ocudu/configs/qos.yml"
    "/home/fyp5g/fyp/ocudu/configs/sibs.yml"
    "/home/fyp5g/fyp/ocudu/configs/slicing.yml"
    "/home/fyp5g/fyp/ocudu/configs/srb.yml"
    "/home/fyp5g/fyp/ocudu/configs/testmode.yml"
    "/home/fyp5g/fyp/ocudu/configs/xnap.yml"
    )
endif()

