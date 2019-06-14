#!/usr/bin/env python3
# Copyright (C) 2016-2018 Intel Corporation
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted for any purpose (including commercial purposes)
# provided that the following conditions are met:
#
# 1. Redistributions of source code must retain the above copyright notice,
#    this list of conditions, and the following disclaimer.
#
# 2. Redistributions in binary form must reproduce the above copyright notice,
#    this list of conditions, and the following disclaimer in the
#    documentation and/or materials provided with the distribution.
#
# 3. In addition, redistributions of modified forms of the source or binary
#    code must carry prominent notices stating that the original code was
#    changed and the date of the change.
#
#  4. All publications or advertising materials mentioning features or use of
#     this software are asked, but not required, to acknowledge that it was
#     developed by Intel Corporation and credit the contributors.
#
# 5. Neither the name of Intel Corporation, nor the name of any Contributor
#    may be used to endorse or promote products derived from this software
#    without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
# ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY
# DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
# (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
# LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
# ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
# THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
# -*- coding: utf-8 -*-
"""
cart rpc to ghost rank test

Usage:

Execute from the install/$arch/TESTING directory.

python3 test_runner scripts/cart_test_rpc_to_ghost_rank.yml

To use valgrind memory checking
set TR_USE_VALGRIND in cart_test_rpc_to_ghost_rank.yml to memcheck

To use valgrind call (callgrind) profiling
set TR_USE_VALGRIND in cart_test_rpc_to_ghost_rank.yml to callgrind


"""

import os
from socket import gethostname

class TestRpcToGhostRank(commontestsuite.CommonTestSuite):
    """ Execute rpc to ghost rank test """
    servers = []
    clients = []

    def setUp(self):
        """setup the test"""
        self.get_test_info()
        log_mask = os.getenv("D_LOG_MASK", "INFO")
        log_file = self.get_cart_long_log_name()
        fault_config = os.getenv("D_FI_CONFIG")
        if not fault_config:
            fault_config = os.path.join(os.getenv('CRT_PREFIX', ".."), "etc", \
                           "fault-inject-cart.yaml")
        crt_phy_addr = os.getenv("CRT_PHY_ADDR_STR", "ofi+sockets")
        ofi_interface = os.getenv("OFI_INTERFACE", "eth0")
        ofi_share_addr = os.getenv("CRT_CTX_SHARE_ADDR", "0")
        ofi_ctx_num = os.getenv("CRT_CTX_NUM", "0")
        self.pass_env = ' -x D_LOG_MASK={!s} -x D_LOG_FILE={!s}' \
                        ' -x CRT_PHY_ADDR_STR={!s}' \
                        ' -x OFI_INTERFACE={!s}' \
                        ' -x CRT_CTX_SHARE_ADDR={!s} -x CRT_CTX_NUM={!s}' \
                        ' -x D_FI_CONFIG={!s}'  \
                            .format(log_mask, log_file, crt_phy_addr, \
                                    ofi_interface, ofi_share_addr, ofi_ctx_num,
                                    fault_config)

    def tearDown(self):
        """tear down the test"""
        self.logger.info("tearDown begin")
        self.logger.info("tearDown end\n")

    def test_rpc_to_ghost_rank_test(self):
        """Simple rpc to ghost rank test"""
        testmsg = self.shortDescription()

        servers = self.get_server_list()
        #run test even if server list is not provided.
        if servers:
            all_servers = ','.join(servers)
            hosts = ''.join([' -H ', all_servers])
        else:
            hosts = ''.join([' -H ', gethostname().split('.')[0]])

        (cmd, prefix) = self.add_prefix_logdir()
        app_str = 'tests/test_rpc_to_ghost_rank' + \
                  ' --name service_group' + \
                  ' --is_service -c 2'
        cmdstr = "{!s} {!s} -np 2 {!s} {!s} {!s}".format(
            cmd, hosts, self.pass_env, prefix, app_str)

        srv_rtn = self.execute_cmd(testmsg, cmdstr)

        if srv_rtn:
            self.fail("rpc to ghost rank test Failed, return code %d" % srv_rtn)
        return srv_rtn
