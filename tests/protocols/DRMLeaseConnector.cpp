// A withdrawn wp_drm_lease_connector_v1 must stay a live protocol object until its client destroys it.
//
// The connector is created by the compositor (new_id in wp_drm_lease_device_v1.connector), so its id comes
// from the server range: the first one a client sees is 0xFF000000. A client answers `withdrawn` by sending
// `destroy` on that id. If the compositor has already destroyed its wl_resource, libwayland finds no object
// there and kills the client with "invalid object 4278190080". Xwayland binds the lease device, so returning
// a `lease`-flagged output to the desktop (CDRMLeaseProtocol::reclaim) used to kill Xwayland exactly so.
//
// These tests drive a real libwayland server with a raw socket client. They pin the connector's lifecycle
// and reproduce the wire-level failure; CDRMLeaseProtocol::reclaim itself needs a DRM backend and a live
// CMonitor, which the unit-test harness does not provide.

#include <protocols/DRMLease.hpp>

#include <gtest/gtest.h>

#include <wayland-server-core.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

static constexpr uint32_t FIRST_SERVER_ID   = 0xFF000000;
static constexpr uint32_t DISPLAY_ID        = 1;
static constexpr uint16_t DISPLAY_ERROR     = 0;
static constexpr uint16_t CONNECTOR_DESTROY = 0; // wp_drm_lease_connector_v1.destroy
static constexpr uint16_t EVENT_WITHDRAWN   = 4; // wp_drm_lease_connector_v1.withdrawn

struct SWireMessage {
    uint32_t    object = 0;
    uint16_t    opcode = 0;
    std::string errorText; // wl_display.error only
};

struct SClientWatch {
    wl_listener listener  = {};
    bool        destroyed = false;
};

static void onClientDestroyed(wl_listener* listener, void* data) {
    SClientWatch* watch = wl_container_of(listener, watch, listener);
    watch->destroyed    = true;
}

class CDRMLeaseConnectorWire : public testing::Test {
  protected:
    void SetUp() override {
        m_display = wl_display_create();
        ASSERT_NE(m_display, nullptr);

        std::array<int, 2> fds = {-1, -1};
        ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds.data()), 0);
        m_clientFD = fds[1];
        fcntl(m_clientFD, F_SETFL, fcntl(m_clientFD, F_GETFL) | O_NONBLOCK);

        m_client = wl_client_create(m_display, fds[0]);
        ASSERT_NE(m_client, nullptr);

        m_watch.listener.notify = onClientDestroyed;
        wl_client_add_destroy_listener(m_client, &m_watch.listener);
    }

    void TearDown() override {
        m_connector.reset();
        if (m_client && !m_watch.destroyed)
            wl_client_destroy(m_client);
        if (m_display)
            wl_display_destroy(m_display);
        if (m_clientFD >= 0)
            close(m_clientFD);
    }

    // what the compositor's sendConnector() does: a server-allocated connector object
    SP<CWpDrmLeaseConnectorV1> makeServerConnector() {
        auto resource = makeShared<CWpDrmLeaseConnectorV1>(m_client, 1, 0);
        return resource;
    }

    void clientSendsDestroy(uint32_t id) {
        const std::array<uint32_t, 2> msg = {id, (8u << 16) | CONNECTOR_DESTROY};
        ASSERT_EQ(write(m_clientFD, msg.data(), sizeof(msg)), sc<ssize_t>(sizeof(msg)));
        wl_event_loop_dispatch(wl_display_get_event_loop(m_display), 0);
    }

    std::vector<SWireMessage> readClientMessages() {
        if (!m_watch.destroyed)
            wl_display_flush_clients(m_display);

        std::vector<uint8_t>      bytes;
        std::array<uint8_t, 4096> buf = {};
        while (true) {
            const auto N = read(m_clientFD, buf.data(), buf.size());
            if (N <= 0)
                break;
            bytes.insert(bytes.end(), buf.begin(), buf.begin() + N);
        }

        std::vector<SWireMessage> out;
        size_t                    off = 0;
        while (off + 8 <= bytes.size()) {
            uint32_t object = 0, sizeOp = 0;
            std::memcpy(&object, &bytes[off], 4);
            std::memcpy(&sizeOp, &bytes[off + 4], 4);
            const uint16_t SIZE = sizeOp >> 16;
            if (SIZE < 8 || off + SIZE > bytes.size())
                break;

            SWireMessage msg{.object = object, .opcode = sc<uint16_t>(sizeOp & 0xFFFF)};
            // wl_display.error: object, uint code, string (uint32 length incl. NUL, then bytes)
            if (object == DISPLAY_ID && msg.opcode == DISPLAY_ERROR && SIZE >= 20) {
                uint32_t len = 0;
                std::memcpy(&len, &bytes[off + 16], 4);
                if (len > 0 && off + 20 + len <= off + SIZE)
                    msg.errorText = std::string(rc<const char*>(&bytes[off + 20]), len - 1);
            }
            out.emplace_back(msg);
            off += SIZE;
        }
        return out;
    }

    static std::optional<SWireMessage> displayError(const std::vector<SWireMessage>& msgs) {
        for (const auto& m : msgs) {
            if (m.object == DISPLAY_ID && m.opcode == DISPLAY_ERROR)
                return m;
        }
        return std::nullopt;
    }

    wl_display*                    m_display  = nullptr;
    wl_client*                     m_client   = nullptr;
    int                            m_clientFD = -1;
    SClientWatch                   m_watch;
    SP<CDRMLeaseConnectorResource> m_connector;
};

TEST_F(CDRMLeaseConnectorWire, ServerCreatedConnectorTakesTheFirstServerId) {
    auto resource = makeServerConnector();
    ASSERT_TRUE(resource->resource());
    EXPECT_EQ(wl_resource_get_id(resource->resource()), FIRST_SERVER_ID);
}

// The fixed contract: withdraw() announces `withdrawn` once and leaves the object alive, so the client's
// `destroy` that follows is a valid request and the client survives.
TEST_F(CDRMLeaseConnectorWire, WithdrawnConnectorSurvivesUntilTheClientDestroysIt) {
    m_connector = makeShared<CDRMLeaseConnectorResource>(WP<CDRMLeaseDeviceResource>{}, makeServerConnector(), PHLMONITOR{});
    ASSERT_TRUE(m_connector->good());
    m_connector->m_self = m_connector;

    m_connector->withdraw();
    m_connector->withdraw(); // idempotent: a second withdraw (e.g. the monitor is later destroyed) sends nothing

    EXPECT_TRUE(m_connector->m_dead);
    EXPECT_TRUE(m_connector->good());
    EXPECT_NE(wl_client_get_object(m_client, FIRST_SERVER_ID), nullptr);

    const auto SENT      = readClientMessages();
    size_t     withdrawn = 0;
    for (const auto& m : SENT) {
        if (m.object == FIRST_SERVER_ID && m.opcode == EVENT_WITHDRAWN)
            withdrawn++;
    }
    EXPECT_EQ(withdrawn, 1u);

    // Xwayland's reaction to `withdrawn`
    clientSendsDestroy(FIRST_SERVER_ID);

    EXPECT_FALSE(m_watch.destroyed);
    EXPECT_FALSE(displayError(readClientMessages()).has_value());
}

// The 2026-10-11 incident on the wire: freeing the withdrawn connector (what reclaim() did by erasing it from
// m_connectors) destroys the server object under the client, and the client's `destroy` is then fatal.
TEST_F(CDRMLeaseConnectorWire, FreeingAWithdrawnConnectorKillsTheClientOnItsDestroy) {
    m_connector = makeShared<CDRMLeaseConnectorResource>(WP<CDRMLeaseDeviceResource>{}, makeServerConnector(), PHLMONITOR{});
    ASSERT_TRUE(m_connector->good());
    m_connector->m_self = m_connector;

    m_connector->withdraw();
    readClientMessages();

    m_connector.reset();
    EXPECT_EQ(wl_client_get_object(m_client, FIRST_SERVER_ID), nullptr);

    clientSendsDestroy(FIRST_SERVER_ID);

    EXPECT_TRUE(m_watch.destroyed);
    const auto ERR = displayError(readClientMessages());
    ASSERT_TRUE(ERR.has_value());
    EXPECT_EQ(ERR->errorText, "invalid object 4278190080");
}
