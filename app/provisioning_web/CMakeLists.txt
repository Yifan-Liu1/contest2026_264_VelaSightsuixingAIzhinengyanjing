# AP-mode provisioning web entry
#
# The public header is exported through the include directory so an application
# can call velasight_provisioning_start() without reaching into this directory.
if(CONFIG_LVX_USE_DEMO_CONTEST2026_264_PROVISION_WEB)
  nuttx_add_library(provision_web_core STATIC)
  target_sources(provision_web_core PRIVATE
    vp_server.c vp_http.c vp_form.c vp_store.c)
  target_include_directories(provision_web_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

  nuttx_add_application(
    NAME provision_web
    SRCS provisioning_web_main.c
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/include
    DEPENDS provision_web_core
    STACKSIZE 4096)
endif()
