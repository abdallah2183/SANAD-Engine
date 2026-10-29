#include <NF/Test/TestFramework.hpp>
#include <NF/Core/EventBus.hpp>

using namespace nf;

struct CustomDamageEvent {
    u32 target_id;
    f32 amount;
};

NF_TEST(eventbus_publish_subscribe) {
    EventBus& bus = EventBus::instance();
    bus.clear();

    int received_count = 0;
    f32 total_damage = 0.0f;

    SubscriptionId sub = bus.subscribe<CustomDamageEvent>([&](const CustomDamageEvent& e) {
        received_count++;
        total_damage += e.amount;
    });

    NF_CHECK(sub != kInvalidSubscriptionId);
    NF_CHECK_EQ(bus.listener_count(), 1ULL);

    bus.publish(CustomDamageEvent{101, 25.5f});
    bus.publish(CustomDamageEvent{102, 14.5f});

    NF_CHECK_EQ(received_count, 2);
    NF_CHECK_NEAR(total_damage, 40.0f, 1e-5f);

    bool unsubbed = bus.unsubscribe<CustomDamageEvent>(sub);
    NF_CHECK(unsubbed);
    NF_CHECK_EQ(bus.listener_count(), 0ULL);

    // Publishing after unsubscribe should not invoke handler
    bus.publish(CustomDamageEvent{103, 10.0f});
    NF_CHECK_EQ(received_count, 2);
    NF_CHECK_NEAR(total_damage, 40.0f, 1e-5f);
}

NF_TEST(eventbus_scoped_subscription) {
    EventBus& bus = EventBus::instance();
    bus.clear();

    int hits = 0;
    {
        ScopedSubscription scope(bus, std::function<void(const WindowResizeEvent&)>([&](const WindowResizeEvent&) {
            hits++;
        }));
        NF_CHECK_EQ(bus.listener_count(), 1ULL);
        bus.publish(WindowResizeEvent{1920, 1080});
        NF_CHECK_EQ(hits, 1);
    }
    // Scope ended, should auto-unsubscribe
    NF_CHECK_EQ(bus.listener_count(), 0ULL);
    bus.publish(WindowResizeEvent{1280, 720});
    NF_CHECK_EQ(hits, 1);
}

NF_TEST(eventbus_queued_events) {
    EventBus& bus = EventBus::instance();
    bus.clear();

    int ticks = 0;
    bus.subscribe<TickEvent>([&](const TickEvent&) {
        ticks++;
    });

    bus.queue(TickEvent{0.016f, 1});
    bus.queue(TickEvent{0.016f, 2});
    bus.queue(TickEvent{0.016f, 3});

    NF_CHECK_EQ(bus.queued_count(), 3ULL);
    NF_CHECK_EQ(ticks, 0); // Not dispatched yet

    bus.dispatch_queued();
    NF_CHECK_EQ(bus.queued_count(), 0ULL);
    NF_CHECK_EQ(ticks, 3);
}
