/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "bgp/evpn/evpn_table.h"

#include <boost/bind.hpp>
#include <tbb/atomic.h>

#include "base/logging.h"
#include "base/task.h"
#include "base/task_annotations.h"
#include "base/test/task_test_util.h"
#include "bgp/bgp_log.h"
#include "bgp/test/bgp_server_test_util.h"
#include "control-node/control_node.h"
#include "io/event_manager.h"
#include "testing/gunit.h"

using namespace std;
using namespace boost;

static const int kRouteCount = 255;

class EvpnTableTest : public ::testing::Test {
protected:
    EvpnTableTest()
        : server_(&evm_), evpn_(NULL) {
    }

    virtual void SetUp() {
        ConcurrencyScope scope("bgp::Config");

        adc_notification_ = 0;
        del_notification_ = 0;

        master_cfg_.reset(
            new BgpInstanceConfig(BgpConfigManager::kMasterInstance));
        server_.routing_instance_mgr()->CreateRoutingInstance(
            master_cfg_.get());
        RoutingInstance *routing_instance =
            server_.routing_instance_mgr()->GetRoutingInstance(
                BgpConfigManager::kMasterInstance);
        ASSERT_TRUE(routing_instance != NULL);

        evpn_ = static_cast<EvpnTable *>(routing_instance->GetTable(Address::EVPN));
        ASSERT_TRUE(evpn_ != NULL);

        tid_ = evpn_->Register(
            boost::bind(&EvpnTableTest::TableListener, this, _1, _2));
    }

    virtual void TearDown() {
        evpn_->Unregister(tid_);
        server_.Shutdown();
        task_util::WaitForIdle();
        TASK_UTIL_ASSERT_EQ(0, server_.routing_instance_mgr()->count());
    }

    void AddRoute(string prefix_str) {
        EvpnPrefix prefix(EvpnPrefix::FromString(prefix_str));

        BgpAttrSpec attrs;
        DBRequest addReq;
        addReq.key.reset(new EvpnTable::RequestKey(prefix, NULL));
        BgpAttrPtr attr = server_.attr_db()->Locate(attrs);
        addReq.data.reset(new EvpnTable::RequestData(attr, 0, 0));
        addReq.oper = DBRequest::DB_ENTRY_ADD_CHANGE;
        evpn_->Enqueue(&addReq);
    }

    void DelRoute(string prefix_str) {
        EvpnPrefix prefix(EvpnPrefix::FromString(prefix_str));

        DBRequest delReq;
        delReq.key.reset(new EvpnTable::RequestKey(prefix, NULL));
        delReq.oper = DBRequest::DB_ENTRY_DELETE;
        evpn_->Enqueue(&delReq);
    }

    void VerifyRouteExists(string prefix_str) {
        EvpnPrefix prefix(EvpnPrefix::FromString(prefix_str));
        EvpnTable::RequestKey key(prefix, NULL);
        EvpnRoute *rt = dynamic_cast<EvpnRoute *>(evpn_->Find(&key));
        TASK_UTIL_EXPECT_TRUE(rt != NULL);
        TASK_UTIL_EXPECT_EQ(1, rt->count());
    }

    void VerifyRouteNoExists(string prefix_str) {
        EvpnPrefix prefix(EvpnPrefix::FromString(prefix_str));
        EvpnTable::RequestKey key(prefix, NULL);
        EvpnRoute *rt = static_cast<EvpnRoute *>(evpn_->Find(&key));
        TASK_UTIL_EXPECT_TRUE(rt == NULL);
    }

    void VerifyTablePartitionCommon(bool empty, int start_idx, int end_idx) {
        if (end_idx == -1)
            end_idx = start_idx + 1;
        TASK_UTIL_EXPECT_TRUE(start_idx < end_idx);
        for (int idx = start_idx; idx < end_idx; ++idx) {
            DBTablePartition *tbl_partition =
                static_cast<DBTablePartition *>(evpn_->GetTablePartition(idx));
            if (empty) {
                TASK_UTIL_EXPECT_EQ(0, tbl_partition->size());
            } else {
                TASK_UTIL_EXPECT_NE(0, tbl_partition->size());
            }
        }
    }

    void VerifyTablePartitionEmpty(int start_idx, int end_idx = -1) {
        VerifyTablePartitionCommon(true, start_idx, end_idx);
    }

    void VerifyTablePartitionNonEmpty(int start_idx, int end_idx = -1) {
        VerifyTablePartitionCommon(false, start_idx, end_idx);
    }

    void TableListener(DBTablePartBase *tpart, DBEntryBase *entry) {
        bool del_notify = entry->IsDeleted();
        if (del_notify) {
            del_notification_++;
        } else {
            adc_notification_++;
        }
    }

    EventManager evm_;
    BgpServer server_;
    EvpnTable *evpn_;
    DBTableBase::ListenerId tid_;
    scoped_ptr<BgpInstanceConfig> master_cfg_;

    tbb::atomic<long> adc_notification_;
    tbb::atomic<long> del_notification_;
};

class EvpnTableAutoDiscoveryTest : public EvpnTableTest {
};

TEST_F(EvpnTableAutoDiscoveryTest, AllocEntryStr) {
    string prefix_str("1-10.1.1.1:65535-00:01:02:03:04:05:06:07:08:09-65536");
    auto_ptr<DBEntry> route = master_->AllocEntryStr(prefix_str);
    EXPECT_EQ(prefix_str, route->ToString());
}

TEST_F(EvpnTableAutoDiscoveryTest, AddDeleteSingleRoute) {
    AddRoute("1-10.1.1.1:65535-00:01:02:03:04:05:06:07:08:09-65536");
    task_util::WaitForIdle();
    VerifyRouteExists("1-10.1.1.1:65535-00:01:02:03:04:05:06:07:08:09-65536");
    TASK_UTIL_EXPECT_EQ(adc_notification_, 1);

    DelRoute("1-10.1.1.1:65535-00:01:02:03:04:05:06:07:08:09-65536");
    task_util::WaitForIdle();
    TASK_UTIL_EXPECT_EQ(del_notification_, 1);
    VerifyRouteNoExists("1-10.1.1.1:65535-00:01:02:03:04:05:06:07:08:09-65536");
}

// Prefixes differ only in the IP address field of the RD.
TEST_F(EvpnTableAutoDiscoveryTest, AddDeleteMultipleRoute1) {
    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "1-10.1.1." << idx << ":65535-";
        repr << "00:01:02:03:04:05:06:07:08:09-65536";
        AddRoute(repr.str());
    }
    task_util::WaitForIdle();

    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "1-10.1.1." << idx << ":65535-";
        repr << "00:01:02:03:04:05:06:07:08:09-65536";
        VerifyRouteExists(repr.str());
    }
    TASK_UTIL_EXPECT_EQ(adc_notification_, kRouteCount);

    VerifyTablePartitionNonEmpty(0);
    VerifyTablePartitionEmpty(1, DB::PartitionCount());

    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "1-10.1.1." << idx << ":65535-";
        repr << "00:01:02:03:04:05:06:07:08:09-65536";
        DelRoute(repr.str());
    }
    task_util::WaitForIdle();

    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "1-10.1.1." << idx << ":65535-";
        repr << "00:01:02:03:04:05:06:07:08:09-65536";
        VerifyRouteNoExists(repr.str());
    }
    TASK_UTIL_EXPECT_EQ(del_notification_, kRouteCount);
}


// Prefixes differ only in the esi field.
TEST_F(EvpnTableAutoDiscoveryTest, AddDeleteMultipleRoute2) {
    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "1-10.1.1.1:65535-";
        repr << "00:01:02:03:04:05:06:07:08:" << hex << idx << "-65536";
        AddRoute(repr.str());
    }
    task_util::WaitForIdle();

    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "1-10.1.1.1:65535-";
        repr << "00:01:02:03:04:05:06:07:08:" << hex << idx << "-65536";
        VerifyRouteExists(repr.str());
    }
    TASK_UTIL_EXPECT_EQ(adc_notification_, kRouteCount);

    VerifyTablePartitionNonEmpty(0);
    VerifyTablePartitionEmpty(1, DB::PartitionCount());

    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "1-10.1.1.1:65535-";
        repr << "00:01:02:03:04:05:06:07:08:" << hex << idx << "-65536";
        DelRoute(repr.str());
    }
    task_util::WaitForIdle();

    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "1-10.1.1.1:65535-";
        repr << "00:01:02:03:04:05:06:07:08:" << hex << idx << "-65536";
        VerifyRouteNoExists(repr.str());
    }
    TASK_UTIL_EXPECT_EQ(del_notification_, kRouteCount);
}

// Prefixes differ only in the tag field.
TEST_F(EvpnTableAutoDiscoveryTest, AddDeleteMultipleRoute3) {
    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "1-10.1.1.1:65535-";
        repr << "00:01:02:03:04:05:06:07:08:09-" << idx;
        AddRoute(repr.str());
    }
    task_util::WaitForIdle();

    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "1-10.1.1.1:65535-";
        repr << "00:01:02:03:04:05:06:07:08:09-" << idx;
        VerifyRouteExists(repr.str());
    }
    TASK_UTIL_EXPECT_EQ(adc_notification_, kRouteCount);

    VerifyTablePartitionNonEmpty(0);
    VerifyTablePartitionEmpty(1, DB::PartitionCount());

    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "1-10.1.1.1:65535-";
        repr << "00:01:02:03:04:05:06:07:08:09-" << idx;
        DelRoute(repr.str());
    }
    task_util::WaitForIdle();

    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "1-10.1.1.1:65535-";
        repr << "00:01:02:03:04:05:06:07:08:09-" << idx;
        VerifyRouteNoExists(repr.str());
    }
    TASK_UTIL_EXPECT_EQ(del_notification_, kRouteCount);
}

class EvpnTableMacAdvertisementTest : public EvpnTableTest {
};

TEST_F(EvpnTableMacAdvertisementTest, AllocEntryStr1) {
    string prefix_str("2-10.1.1.1:65535-0-11:12:13:14:15:16,192.168.1.1");
    auto_ptr<DBEntry> route = master_->AllocEntryStr(prefix_str);
    EXPECT_EQ(prefix_str, route->ToString());
}

TEST_F(EvpnTableMacAdvertisementTest, AllocEntryStr2) {
    string prefix_str("2-10.1.1.1:65535-100000-11:12:13:14:15:16,192.168.1.1");
    auto_ptr<DBEntry> route = master_->AllocEntryStr(prefix_str);
    EXPECT_EQ(prefix_str, route->ToString());
}

TEST_F(EvpnTableMacAdvertisementTest, AllocEntryStr3) {
    string prefix_str("2-10.1.1.1:65535-100000-11:12:13:14:15:16,0.0.0.0");
    auto_ptr<DBEntry> route = master_->AllocEntryStr(prefix_str);
    EXPECT_EQ(prefix_str, route->ToString());
}

TEST_F(EvpnTableMacAdvertisementTest, AllocEntryStr4) {
    string prefix_str("2-10.1.1.1:65535-0-11:12:13:14:15:16,2001:db8:0:9::1");
    auto_ptr<DBEntry> route = master_->AllocEntryStr(prefix_str);
    EXPECT_EQ(prefix_str, route->ToString());
}

TEST_F(EvpnTableMacAdvertisementTest, AddDeleteSingleRoute) {
    AddRoute("2-10.1.1.1:65535-0-11:12:13:14:15:16,192.168.1.1");
    task_util::WaitForIdle();
    VerifyRouteExists("2-10.1.1.1:65535-0-11:12:13:14:15:16,192.168.1.1");
    TASK_UTIL_EXPECT_EQ(adc_notification_, 1);

    DelRoute("2-10.1.1.1:65535-0-11:12:13:14:15:16,192.168.1.1");
    task_util::WaitForIdle();
    TASK_UTIL_EXPECT_EQ(del_notification_, 1);
    VerifyRouteNoExists("2-10.1.1.1:65535-0-11:12:13:14:15:16,192.168.1.1");
}

// Prefixes differ only in the IP address field of the RD.
TEST_F(EvpnTableMacAdvertisementTest, AddDeleteMultipleRoute1) {
    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "2-10.1.1." << idx << ":65535-";
        repr << "0-07:00:00:00:00:01,192.168.1.1";
        AddRoute(repr.str());
    }
    task_util::WaitForIdle();

    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "2-10.1.1." << idx << ":65535-";
        repr << "0-07:00:00:00:00:01,192.168.1.1";
        VerifyRouteExists(repr.str());
    }
    TASK_UTIL_EXPECT_EQ(adc_notification_, kRouteCount);

    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "2-10.1.1." << idx << ":65535-";
        repr << "0-07:00:00:00:00:01,192.168.1.1";
        DelRoute(repr.str());
    }
    task_util::WaitForIdle();

    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "2-10.1.1." << idx << ":65535-";
        repr << "0-07:00:00:00:00:01,192.168.1.1";
        VerifyRouteNoExists(repr.str());
    }
    TASK_UTIL_EXPECT_EQ(del_notification_, kRouteCount);
}


// Prefixes differ only in the mac_addr field.
TEST_F(EvpnTableMacAdvertisementTest, AddDeleteMultipleRoute2) {
    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "2-10.1.1.1:65535-";
        repr << "0-07:00:00:00:00:" << hex << idx << ",192.168.1.1";
        AddRoute(repr.str());
    }
    task_util::WaitForIdle();

    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "2-10.1.1.1:65535-";
        repr << "0-07:00:00:00:00:" << hex << idx << ",192.168.1.1";
        VerifyRouteExists(repr.str());
    }
    TASK_UTIL_EXPECT_EQ(adc_notification_, kRouteCount);

    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "2-10.1.1.1:65535-";
        repr << "0-07:00:00:00:00:" << hex << idx << ",192.168.1.1";
        DelRoute(repr.str());
    }
    task_util::WaitForIdle();

    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "2-10.1.1.1:65535-";
        repr << "0-07:00:00:00:00:" << hex << idx << ",192.168.1.1";
        VerifyRouteNoExists(repr.str());
    }
    TASK_UTIL_EXPECT_EQ(del_notification_, kRouteCount);
}

// Prefixes differ only in the ip_addr field.
TEST_F(EvpnTableMacAdvertisementTest, AddDeleteMultipleRoute3) {
    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "2-10.1.1.1:65535-";
        repr << "0-07:00:00:00:00:01,192.168.1." << idx;
        AddRoute(repr.str());
    }
    task_util::WaitForIdle();

    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "2-10.1.1.1:65535-";
        repr << "0-07:00:00:00:00:01,192.168.1." << idx;
        VerifyRouteExists(repr.str());
    }
    TASK_UTIL_EXPECT_EQ(adc_notification_, kRouteCount);

    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "2-10.1.1.1:65535-";
        repr << "0-07:00:00:00:00:01,192.168.1." << idx;
        DelRoute(repr.str());
    }
    task_util::WaitForIdle();

    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "2-10.1.1.1:65535-";
        repr << "0-07:00:00:00:00:01,192.168.1." << idx;
        VerifyRouteNoExists(repr.str());
    }
    TASK_UTIL_EXPECT_EQ(del_notification_, kRouteCount);
}

TEST_F(EvpnTableMacAdvertisementTest, Hashing) {
    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "2-10.1.1.1:65535-";
        repr << "0-07:00:00:00:00:" << hex << idx << ",192.168.1.1";
        AddRoute(repr.str());
    }
    task_util::WaitForIdle();

    VerifyTablePartitionNonEmpty(0, DB::PartitionCount());

    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "2-10.1.1.1:65535-";
        repr << "0-07:00:00:00:00:" << hex << idx << ",192.168.1.1";
        DelRoute(repr.str());
    }
    task_util::WaitForIdle();
}

class EvpnTableInclusiveMulticastTest : public EvpnTableTest {
};

TEST_F(EvpnTableInclusiveMulticastTest, AllocEntryStr1) {
    string prefix_str("3-10.1.1.1:65535-0-192.1.1.1");
    auto_ptr<DBEntry> route = master_->AllocEntryStr(prefix_str);
    EXPECT_EQ(prefix_str, route->ToString());
}

TEST_F(EvpnTableInclusiveMulticastTest, AllocEntryStr2) {
    string prefix_str("3-10.1.1.1:65535-65536-192.1.1.1");
    auto_ptr<DBEntry> route = master_->AllocEntryStr(prefix_str);
    EXPECT_EQ(prefix_str, route->ToString());
}

TEST_F(EvpnTableInclusiveMulticastTest, AllocEntryStr3) {
    string prefix_str("3-10.1.1.1:65535-65536-2001:db8:0:9::1");
    auto_ptr<DBEntry> route = master_->AllocEntryStr(prefix_str);
    EXPECT_EQ(prefix_str, route->ToString());
}

TEST_F(EvpnTableInclusiveMulticastTest, AddDeleteSingleRoute) {
    AddRoute("3-10.1.1.1:65535-65536-192.1.1.1");
    task_util::WaitForIdle();
    VerifyRouteExists("3-10.1.1.1:65535-65536-192.1.1.1");
    TASK_UTIL_EXPECT_EQ(adc_notification_, 1);

    DelRoute("3-10.1.1.1:65535-65536-192.1.1.1");
    task_util::WaitForIdle();
    TASK_UTIL_EXPECT_EQ(del_notification_, 1);
    VerifyRouteNoExists("3-10.1.1.1:65535-65536-192.1.1.1");
}

// Prefixes differ only in the IP address field of the RD.
TEST_F(EvpnTableInclusiveMulticastTest, AddDeleteMultipleRoute1) {
    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "3-10.1.1." << idx << ":65535-65536-192.1.1.1";
        AddRoute(repr.str());
    }
    task_util::WaitForIdle();

    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "3-10.1.1." << idx << ":65535-65536-192.1.1.1";
        VerifyRouteExists(repr.str());
    }
    TASK_UTIL_EXPECT_EQ(adc_notification_, kRouteCount);

    VerifyTablePartitionNonEmpty(0);
    VerifyTablePartitionEmpty(1, DB::PartitionCount());

    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "3-10.1.1." << idx << ":65535-65536-192.1.1.1";
        DelRoute(repr.str());
    }
    task_util::WaitForIdle();

    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "3-10.1.1." << idx << ":65535-65536-192.1.1.1";
        VerifyRouteNoExists(repr.str());
    }
    TASK_UTIL_EXPECT_EQ(del_notification_, kRouteCount);
}


// Prefixes differ only in the tag field.
TEST_F(EvpnTableInclusiveMulticastTest, AddDeleteMultipleRoute2) {
    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "3-10.1.1.1:65535-" << idx << "-192.1.1.1";
        AddRoute(repr.str());
    }
    task_util::WaitForIdle();

    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "3-10.1.1.1:65535-" << idx << "-192.1.1.1";
        VerifyRouteExists(repr.str());
    }
    TASK_UTIL_EXPECT_EQ(adc_notification_, kRouteCount);

    VerifyTablePartitionNonEmpty(0);
    VerifyTablePartitionEmpty(1, DB::PartitionCount());

    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "3-10.1.1.1:65535-" << idx << "-192.1.1.1";
        DelRoute(repr.str());
    }
    task_util::WaitForIdle();

    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "3-10.1.1.1:65535-" << idx << "-192.1.1.1";
        VerifyRouteNoExists(repr.str());
    }
    TASK_UTIL_EXPECT_EQ(del_notification_, kRouteCount);
}

// Prefixes differ only in the IP address field.
TEST_F(EvpnTableInclusiveMulticastTest, AddDeleteMultipleRoute3) {
    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "3-10.1.1.1:65535-65536-192.1.1." << idx;
        AddRoute(repr.str());
    }
    task_util::WaitForIdle();

    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "3-10.1.1.1:65535-65536-192.1.1." << idx;
        VerifyRouteExists(repr.str());
    }
    TASK_UTIL_EXPECT_EQ(adc_notification_, kRouteCount);

    VerifyTablePartitionNonEmpty(0);
    VerifyTablePartitionEmpty(1, DB::PartitionCount());

    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "3-10.1.1.1:65535-65536-192.1.1." << idx;
        DelRoute(repr.str());
    }
    task_util::WaitForIdle();

    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "3-10.1.1.1:65535-65536-192.1.1." << idx;
        VerifyRouteNoExists(repr.str());
    }
    TASK_UTIL_EXPECT_EQ(del_notification_, kRouteCount);
}

class EvpnTableSegmentTest : public EvpnTableTest {
};

TEST_F(EvpnTableSegmentTest, AllocEntryStr1) {
    string prefix_str(
        "4-10.1.1.1:65535-00:01:02:03:04:05:06:07:08:09-192.1.1.1");
    auto_ptr<DBEntry> route = master_->AllocEntryStr(prefix_str);
    EXPECT_EQ(prefix_str, route->ToString());
}

TEST_F(EvpnTableSegmentTest, AllocEntryStr2) {
    string prefix_str(
        "4-10.1.1.1:65535-00:01:02:03:04:05:06:07:08:09-2001:db8:0:9::1");
    auto_ptr<DBEntry> route = master_->AllocEntryStr(prefix_str);
    EXPECT_EQ(prefix_str, route->ToString());
}

TEST_F(EvpnTableSegmentTest, AddDeleteSingleRoute) {
    AddRoute("4-10.1.1.1:65535-00:01:02:03:04:05:06:07:08:09-192.1.1.1");
    task_util::WaitForIdle();
    VerifyRouteExists("4-10.1.1.1:65535-00:01:02:03:04:05:06:07:08:09-192.1.1.1");
    TASK_UTIL_EXPECT_EQ(adc_notification_, 1);

    DelRoute("4-10.1.1.1:65535-00:01:02:03:04:05:06:07:08:09-192.1.1.1");
    task_util::WaitForIdle();
    TASK_UTIL_EXPECT_EQ(del_notification_, 1);
    VerifyRouteNoExists("4-10.1.1.1:65535-00:01:02:03:04:05:06:07:08:09-192.1.1.1");
}

// Prefixes differ only in the IP address field of the RD.
TEST_F(EvpnTableSegmentTest, AddDeleteMultipleRoute1) {
    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "4-10.1.1." << idx << ":65535-";
        repr << "00:01:02:03:04:05:06:07:08:09-192.1.1.1";
        AddRoute(repr.str());
    }
    task_util::WaitForIdle();

    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "4-10.1.1." << idx << ":65535-";
        repr << "00:01:02:03:04:05:06:07:08:09-192.1.1.1";
        VerifyRouteExists(repr.str());
    }
    TASK_UTIL_EXPECT_EQ(adc_notification_, kRouteCount);

    VerifyTablePartitionNonEmpty(0);
    VerifyTablePartitionEmpty(1, DB::PartitionCount());

    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "4-10.1.1." << idx << ":65535-";
        repr << "00:01:02:03:04:05:06:07:08:09-192.1.1.1";
        DelRoute(repr.str());
    }
    task_util::WaitForIdle();

    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "4-10.1.1." << idx << ":65535-";
        repr << "00:01:02:03:04:05:06:07:08:09-192.1.1.1";
        VerifyRouteNoExists(repr.str());
    }
    TASK_UTIL_EXPECT_EQ(del_notification_, kRouteCount);
}


// Prefixes differ only in the esi field.
TEST_F(EvpnTableSegmentTest, AddDeleteMultipleRoute2) {
    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "4-10.1.1.1:65535-";
        repr << "00:01:02:03:04:05:06:07:08:" << hex << idx << "-192.1.1.1";
        AddRoute(repr.str());
    }
    task_util::WaitForIdle();

    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "4-10.1.1.1:65535-";
        repr << "00:01:02:03:04:05:06:07:08:" << hex << idx << "-192.1.1.1";
        VerifyRouteExists(repr.str());
    }
    TASK_UTIL_EXPECT_EQ(adc_notification_, kRouteCount);

    VerifyTablePartitionNonEmpty(0);
    VerifyTablePartitionEmpty(1, DB::PartitionCount());

    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "4-10.1.1.1:65535-";
        repr << "00:01:02:03:04:05:06:07:08:" << hex << idx << "-192.1.1.1";
        DelRoute(repr.str());
    }
    task_util::WaitForIdle();

    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "4-10.1.1.1:65535-";
        repr << "00:01:02:03:04:05:06:07:08:" << hex << idx << "-192.1.1.1";
        VerifyRouteNoExists(repr.str());
    }
    TASK_UTIL_EXPECT_EQ(del_notification_, kRouteCount);
}

// Prefixes differ only in the IP address field.
TEST_F(EvpnTableSegmentTest, AddDeleteMultipleRoute3) {
    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "4-10.1.1.1:65535-";
        repr << "00:01:02:03:04:05:06:07:08:09-192.1.1." << idx;
        AddRoute(repr.str());
    }
    task_util::WaitForIdle();

    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "4-10.1.1.1:65535-";
        repr << "00:01:02:03:04:05:06:07:08:09-192.1.1." << idx;
        VerifyRouteExists(repr.str());
    }
    TASK_UTIL_EXPECT_EQ(adc_notification_, kRouteCount);

    VerifyTablePartitionNonEmpty(0);
    VerifyTablePartitionEmpty(1, DB::PartitionCount());

    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "4-10.1.1.1:65535-";
        repr << "00:01:02:03:04:05:06:07:08:09-192.1.1." << idx;
        DelRoute(repr.str());
    }
    task_util::WaitForIdle();

    for (int idx = 1; idx <= kRouteCount; idx++) {
        ostringstream repr;
        repr << "4-10.1.1.1:65535-";
        repr << "00:01:02:03:04:05:06:07:08:09-192.1.1." << idx;
        VerifyRouteNoExists(repr.str());
    }
    TASK_UTIL_EXPECT_EQ(del_notification_, kRouteCount);
}

int main(int argc, char **argv) {
    bgp_log_test::init();
    ::testing::InitGoogleTest(&argc, argv);
    ControlNode::SetDefaultSchedulingPolicy();
    int result = RUN_ALL_TESTS();
    TaskScheduler::GetInstance()->Terminate();
    return result;
}
