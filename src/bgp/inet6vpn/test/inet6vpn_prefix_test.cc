/*
 * Copyright (c) 2014 Juniper Networks, Inc. All rights reserved.
 */

#include "bgp/inet6vpn/inet6vpn_route.h"

#include "base/logging.h"
#include "base/task.h"
#include "bgp/bgp_log.h"
#include "bgp/inet6/inet6_route.h"
#include "control-node/control_node.h"
#include "testing/gunit.h"

class Inet6PrefixTest : public ::testing::Test {
    virtual void SetUp() {
        Inet6Masks::Init();
    }
    virtual void TearDown() {
        Inet6Masks::Clear();
    }
};

TEST_F(Inet6PrefixTest, BuildPrefix) {
    boost::system::error_code ec;

    // type 0 - 2B asn, 4B local
    Inet6VpnPrefix prefix(Inet6VpnPrefix::FromString(
        "65412:4294967295:2001:0db8:85a3:0000:0000:8a2e:0370:7334/64", &ec));
    EXPECT_EQ(prefix.ToString(),
              "65412:4294967295:2001:db8:85a3::8a2e:370:7334/64");
    EXPECT_EQ(ec.value(), 0);

    // type 0 - asn 0 is invalid
    prefix = Inet6VpnPrefix::FromString(
        "0:4294967295:2001:0db8:85a3:0000:0000:8a2e:0370:7334/64", &ec);
    EXPECT_NE(ec.value(), 0);

    // type 0 - asn 65535 is invalid
    prefix = Inet6VpnPrefix::FromString(
        "65535:4294967295:2001:0db8:85a3:0000:0000:8a2e:0370:7334/64", &ec);
    EXPECT_NE(ec.value(), 0);

    // type 0 - asn 88888 is invalid
    prefix = Inet6VpnPrefix::FromString(
        "88888:4294967295:2001:0db8:85a3:0000:0000:8a2e:0370:7334/64", &ec);
    EXPECT_NE(ec.value(), 0);

    // type 0 - assigned local 4294967296 is invalid
    prefix = Inet6VpnPrefix::FromString(
        "100:4294967296:2001:0db8:85a3:0000:0000:8a2e:0370:7334/64", &ec);
    EXPECT_NE(ec.value(), 0);

    // type 1 - 4B ip address, 2B local
    prefix = Inet6VpnPrefix::FromString(
        "10.1.1.1:4567:2001:0db8:85a3:0000:0000:8a2e:0370:7334/64", &ec);
    EXPECT_EQ(prefix.ToString(),
              "10.1.1.1:4567:2001:db8:85a3::8a2e:370:7334/64");
    EXPECT_EQ(ec.value(), 0);

    // type 1 with assigned as 0
    prefix = Inet6VpnPrefix::FromString(
        "10.1.1.1:0:2001:0db8:85a3:0000:0000:8a2e:0370:7334/64", &ec);
    EXPECT_EQ(prefix.ToString(),
              "10.1.1.1:0:2001:db8:85a3::8a2e:370:7334/64");
    EXPECT_EQ(ec.value(), 0);

    // type 1 with assigned as 65535
    prefix = Inet6VpnPrefix::FromString(
        "10.1.1.1:65535:2001:0db8:85a3:0000:0000:8a2e:0370:7334/64", &ec);
    EXPECT_EQ(prefix.ToString(),
              "10.1.1.1:65535:2001:db8:85a3::8a2e:370:7334/64");
    EXPECT_EQ(ec.value(), 0);

    // type 1 - assinged local 65536 is invalid
    prefix = Inet6VpnPrefix::FromString(
        "10.1.1.1:65536:2001:0db8:85a3:0000:0000:8a2e:0370:7334/64", &ec);
    EXPECT_NE(ec.value(), 0);

    /* type2 not supported?
    // type 2 - 4B asn, 2B local
    prefix = Inet6VpnPrefix::FromString(
        "4294967295:65412:2001:0db8:85a3::8a2e:0370:7334/64", &ec);
    EXPECT_EQ(prefix.ToString(),
              "4294967295:65412:2001:db8:85a3::8a2e:370:7334/64");
    EXPECT_EQ(ec.value(), 0);
    */
}

TEST_F(Inet6PrefixTest, MoreSpecificTest) {
    boost::system::error_code ec;

    Inet6VpnPrefix phost1(Inet6VpnPrefix::FromString(
        "65412:4294967295:2001:0db8:85a3:0000:0000:8a2e:0370:7334/80", &ec));
    EXPECT_EQ(phost1.ToString(),
              "65412:4294967295:2001:db8:85a3::8a2e:370:7334/80");
    EXPECT_EQ(ec.value(), 0);

    Inet6VpnPrefix pnet1_1(Inet6VpnPrefix::FromString(
        "65412:4294967295:2001:0db8:85a3:0000:0000:8a2e:0370:7334/64", &ec));
    EXPECT_EQ(pnet1_1.ToString(),
              "65412:4294967295:2001:db8:85a3::8a2e:370:7334/64");
    EXPECT_EQ(ec.value(), 0);

    EXPECT_EQ(phost1.IsMoreSpecific(pnet1_1), true);

    // Smaller RD value should not matter since only ip's are compared
    Inet6VpnPrefix pnet1_2(Inet6VpnPrefix::FromString(
        "1:1:2001:0db8:85a3:0000:0000:8a2e:0370:7334/64", &ec));
    EXPECT_EQ(pnet1_2.ToString(),
              "1:1:2001:db8:85a3::8a2e:370:7334/64");
    EXPECT_EQ(ec.value(), 0);

    EXPECT_EQ(phost1.IsMoreSpecific(pnet1_2), true);

    // pnet1_1 vs itself
    EXPECT_EQ(pnet1_1.IsMoreSpecific(pnet1_1), true);
}

int main(int argc, char **argv) {
    bgp_log_test::init();
    ::testing::InitGoogleTest(&argc, argv);
    ControlNode::SetDefaultSchedulingPolicy();
    int result = RUN_ALL_TESTS();
    TaskScheduler::GetInstance()->Terminate();
    return result;
}
