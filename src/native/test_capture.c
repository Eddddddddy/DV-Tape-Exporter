/* Offline callback ownership regression: no device or USB context is opened. */
#define UVCDV_NO_ENTRY
#define libusb_free_transfer test_free_transfer
#define libusb_submit_transfer test_submit_transfer
#include "uvcdv-capture.c"
#undef libusb_free_transfer
#undef libusb_submit_transfer

#include <assert.h>
extern void LIBUSB_CALL libusb_free_transfer(struct libusb_transfer *transfer);
static int test_submit_result = 0;
static int test_frees = 0;

int LIBUSB_CALL test_submit_transfer(struct libusb_transfer *transfer)
{
    (void)transfer;
    return test_submit_result;
}

void LIBUSB_CALL test_free_transfer(struct libusb_transfer *transfer)
{
    /* The cancellation table must be invalidated before any free. */
    struct libusb_transfer **slot = transfer->user_data;
    if (slot) assert(*slot == NULL);
    test_frees++;
    libusb_free_transfer(transfer);
}

static struct libusb_transfer *make_transfer(struct libusb_transfer **slot)
{
    struct libusb_transfer *transfer = libusb_alloc_transfer(1);
    assert(transfer);
    transfer->num_iso_packets = 1;
    transfer->iso_packet_desc[0].status = LIBUSB_TRANSFER_COMPLETED;
    transfer->iso_packet_desc[0].actual_length = 0;
    transfer->user_data = slot;
    *slot = transfer;
    g_inflight = 1;
    g_stop = g_write_error = g_transport_failed = g_device_lost = 0;
    g_stop_reason = STOP_NONE;
    return transfer;
}

static void test_transport(void)
{
    g_stop = 0; g_stop_reason = STOP_NONE; g_bytes = 144000;
    g_observed_play = g_confirmed_end_reads = g_physical_end_verified = 0;
    g_camera_stop_since_ms = 0; g_stop_after_camera_stop = 20;
    observe_transport(1, XPRT_STOP, 1000, 1000);
    observe_transport(1, XPRT_STOP, 24000, 24000);
    assert(!g_stop); /* Existing stopped camera was never played in this run. */
    observe_transport(1, XPRT_PLAY_FORWARD, 25000, 25000);
    observe_transport(1, XPRT_STOP, 26000, 26000);
    observe_transport(1, XPRT_STOP, 45999, 45999);
    assert(!g_stop);
    observe_transport(LIBUSB_ERROR_TIMEOUT, 0, 46000, 46000);
    observe_transport(1, XPRT_STOP, 47000, 47000);
    assert(!g_stop); /* Failed state read resets the consecutive stop interval. */
    observe_transport(1, XPRT_STOP, 67000, 67000);
    assert(g_stop && g_stop_reason == STOP_CAMERA_STOPPED && !g_physical_end_verified);
    g_stop = 0; g_stop_reason = STOP_NONE; g_confirmed_end_reads = 0;
    observe_transport(1, 0x75, 68000, 9000);
    observe_transport(1, 0x75, 69000, 10000);
    assert(!g_stop && !g_physical_end_verified);
    observe_transport(LIBUSB_ERROR_PIPE, 0, 70000, 11000);
    observe_transport(1, 0x75, 71000, 12000);
    assert(!g_stop);
    observe_transport(1, 0x75, 76000, 17000);
    assert(g_stop && g_stop_reason == STOP_PHYSICAL_END && g_physical_end_verified);
    g_stop = 0; g_stop_reason = STOP_NONE; g_confirmed_end_reads = 0; g_physical_end_verified = 0;
    g_bytes = 0;
    observe_transport(1, 0x75, 100000, 100000);
    observe_transport(1, 0x75, 105000, 105000);
    assert(!g_stop && !g_physical_end_verified); /* No captured data cannot certify end. */
    puts("PASS: ordinary Stop64 requires observed play and 20s continuous successful reads; only two 0x75 reads verify physical end.");
}

#ifndef _WIN32
static void test_stdin(void)
{
    int saved = dup(STDIN_FILENO), descriptors[2];
    assert(saved >= 0 && pipe(descriptors) == 0);
    assert(dup2(descriptors[0], STDIN_FILENO) >= 0);
    close(descriptors[0]);
    g_stop = g_stdin_done = 0; g_stop_reason = STOP_NONE;
    assert(write(descriptors[1], "ignore\nstop\n", 12) == 12);
    stdin_control_main(NULL);
    assert(g_stop && g_stop_reason == STOP_STDIN);
    close(descriptors[1]);
    g_stop = g_stdin_done = 0; g_stop_reason = STOP_NONE;
    stdin_control_main(NULL);
    assert(g_stop && g_stop_reason == STOP_PARENT_EOF);
    assert(dup2(saved, STDIN_FILENO) >= 0); close(saved);
    puts("PASS: stop line and parent EOF request graceful shutdown without USB calls.");
}
#endif

int main(void)
{
    (void)uvcdv_main; /* Compile the real CLI without opening USB in this test. */
    struct libusb_transfer *slot = NULL;
    struct libusb_transfer *transfer = make_transfer(&slot);
    transfer->status = LIBUSB_TRANSFER_NO_DEVICE;
    xfer_cb(transfer);
    assert(slot == NULL && g_inflight == 0);
    assert(g_transport_failed && g_device_lost && g_stop);

    transfer = make_transfer(&slot);
    transfer->status = LIBUSB_TRANSFER_CANCELLED;
    xfer_cb(transfer);
    assert(slot == NULL && g_inflight == 0 && !g_transport_failed);

    transfer = make_transfer(&slot);
    transfer->status = LIBUSB_TRANSFER_COMPLETED;
    g_stop = 1;
    xfer_cb(transfer);
    assert(slot == NULL && g_inflight == 0);

    transfer = make_transfer(&slot);
    transfer->status = LIBUSB_TRANSFER_COMPLETED;
    test_submit_result = 0;
    xfer_cb(transfer);
    assert(slot == transfer && g_inflight == 1);
    free_owned_transfer(transfer);
    assert(slot == NULL);

    /* A completed payload is accepted before a failed requeue, and the
     * raw writer drains those already accepted bytes on clean shutdown. */
    assert(ring_init(&g_ring, 1024) == 0);
    g_out = tmpfile();
    assert(g_out);
    g_raw_on = 1;
    g_bytes = g_written = 0;
    pthread_t writer;
    assert(pthread_create(&writer, NULL, writer_main, &g_ring) == 0);
    transfer = make_transfer(&slot);
    unsigned char *buffer = malloc(8);
    assert(buffer);
    memcpy(buffer, "\x02\x00" "ABCDEF", 8);
    libusb_fill_iso_transfer(transfer, NULL, 0x81, buffer, 8, 1, xfer_cb, &slot, 1000);
    transfer->flags |= LIBUSB_TRANSFER_FREE_BUFFER;
    transfer->status = LIBUSB_TRANSFER_COMPLETED;
    transfer->iso_packet_desc[0].status = LIBUSB_TRANSFER_COMPLETED;
    transfer->iso_packet_desc[0].actual_length = 8;
    transfer->iso_packet_desc[0].length = 8;
    test_submit_result = LIBUSB_ERROR_NO_DEVICE;
    xfer_cb(transfer);
    assert(slot == NULL && g_inflight == 0 && g_transport_failed && g_device_lost);
    ring_set_eof(&g_ring);
    pthread_join(writer, NULL);
    fflush(g_out);
    rewind(g_out);
    char actual[6];
    assert(fread(actual, 1, 6, g_out) == 6);
    assert(memcmp(actual, "ABCDEF", 6) == 0);
    assert(g_bytes == 6 && g_written == 6 && g_ring.ovf == 0);
    fclose(g_out);
    free(g_ring.buf);
    pthread_cond_destroy(&g_ring.cv);
    pthread_mutex_destroy(&g_ring.mu);
    assert(test_frees == 5);
    puts("PASS: NO_DEVICE/cancel/stop/requeue clear owner slots; accepted raw data drains fully.");
    test_transport();
#ifndef _WIN32
    test_stdin();
#endif
    return 0;
}
