# SPDX-License-Identifier: Apache-2.0
add_vpp_plugin(media_server
  SOURCES
  media_server.c

  LINK_LIBRARIES
  vnet vppinfra
)
