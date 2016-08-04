/*
 * A sysbus device that is relaying all operations.
 *
 * Copyright (c) 2016 Jonas Zaddach <zaddach@eurecom.fr>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "sysbus.h"
#include "qemu-char.h"
#include "qint.h"
#include "qdict.h"
#include "qstring.h"
#include "qjson.h"
#include "qemu-thread.h"
#include "memory.h"
#include "exec-memory.h"

typedef struct QueueItem
{
    void *data;
    struct QueueItem *next;
} QueueItem;

typedef struct Queue
{
    QueueItem *head;
    QueueItem *tail;
    QemuCond cond;
    QemuMutex mutex;
} Queue;

static void queue_enqueue(Queue *queue, void *data) {
    QueueItem *item = (QueueItem *) malloc(sizeof(QueueItem));
    
    item->data = data;
    item->next = NULL;
    if (queue->tail) {
        queue->tail->next = item;
    }
    queue->tail = item;
}

static void* queue_dequeue(Queue *queue, bool *empty) {
    QueueItem *item = queue->head;
    if (queue->head) {
        queue->head = queue->head->next;
        if (queue->head == NULL) {
            queue->tail = NULL;
        }
    }
    if (empty) {
        *empty = item == NULL;
    }

    if (item) {
        void *data = item->data;
        free(item);
        return data;
    }
    else {
        return NULL;
    }
}
    
#define MAX_RECEIVE_BUF_SIZE 256
    

typedef struct AvatarState
{
    SysBusDevice busdev;
    MemoryRegion mmio;
    CharDriverState *chr;
    qemu_irq irq;

    uint32_t mmio_size;
    uint32_t mmio_address;
    char *chardev_file;

    Queue responses;
    char receive_buf[MAX_RECEIVE_BUF_SIZE];
    int receive_buf_pos;
} AvatarState;


static uint64_t
mmio_read(void *opaque, target_phys_addr_t addr, unsigned int size)
{
    AvatarState *s = opaque;
    QDict *msg = qdict_new();
    QString *msg_str;

    assert(msg && "Failed to allocate memory for JSON dictionary");
    
    qdict_put(msg, "address", qint_from_int((int64_t) addr));
    qdict_put(msg, "size", qint_from_int(size));

    msg_str = qobject_to_json(QOBJECT(msg));
    
    qemu_chr_fe_write(s->chr, (const uint8_t *) qstring_get_str(msg_str), msg_str->length);

    QDECREF(msg_str);
    QDECREF(msg);

    //TODO: Wait for queue to signal that a new item has arrived
    qemu_mutex_lock(&s->responses.mutex);
    qemu_cond_wait(&s->responses.cond, &s->responses.mutex);
    bool empty;
    QDict* response = (QDict*) queue_dequeue(&s->responses, &empty);
    assert(!empty && "Expected item in item queue");
    assert(response && "Response dictionary is NULL");

    qemu_mutex_unlock(&s->responses.mutex);

    uint64_t value = qdict_get_int(response, "value");
    QDECREF(response);
    
    return value;
}

static void
mmio_write(void *opaque, target_phys_addr_t addr,
           uint64_t val64, unsigned int size)
{
    AvatarState *s = opaque;
    QDict *msg = qdict_new();
    QString *msg_str;

    assert(msg && "Failed to allocate memory for JSON dictionary");
    
    qdict_put(msg, "address", qint_from_int((int64_t) addr));
    qdict_put(msg, "size", qint_from_int(size));
    qdict_put(msg, "value", qint_from_int((int64_t) val64));

    msg_str = qobject_to_json(QOBJECT(msg));
    
    qemu_chr_fe_write(s->chr, (const uint8_t*) qstring_get_str(msg_str), msg_str->length);

    QDECREF(msg_str);
    QDECREF(msg);
}

static const MemoryRegionOps mmio_ops = {
    .read = mmio_read,
    .write = mmio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4
    }
};

static void chardev_rx(void *opaque, const uint8_t *buf, int size)
{
    AvatarState  *s = opaque;
    unsigned i;

    for (i = 0; i < size; ++i) {
        if (buf[i] == '\n') {
            //TODO: Do we have to hold a lock while modifying the receive_buf?
            QObject *obj = qobject_from_json(s->receive_buf);
            if (obj) {
                QDict *dict = qobject_to_qdict(obj);
                assert(dict && "Must be a QDict");

                qemu_mutex_lock(&s->responses.mutex);
                queue_enqueue(&s->responses, dict);
                qemu_cond_signal(&s->responses.cond);
                qemu_mutex_unlock(&s->responses.mutex);
            } 
            else {
                //TODO: Error, cannot parse response
            }
            s->receive_buf_pos = 0;
        }
        else { 
            assert(s->receive_buf_pos < MAX_RECEIVE_BUF_SIZE && "Receive buffer overflow");
            s->receive_buf[s->receive_buf_pos++] = buf[i];
        }
    }
}

static int chardev_can_rx(void *opaque)
{
    /* We don't really care how many bytes they give us */
    return 256;
}

static void chardev_event(void *opaque, int event)
{

}

static int avatar_device_init(SysBusDevice *dev)
{
    AvatarState *s = FROM_SYSBUS(AvatarState, dev);

    s->receive_buf_pos = 0;
    s->responses.head = NULL;
    s->responses.tail = NULL;
    qemu_mutex_init(&s->responses.mutex);
    qemu_cond_init(&s->responses.cond);

//    s->chr = qemu_chr_new("avatar", s->chardev_file, NULL);

    if (!s->chr) {
        printf("avatar: Can't create Avatar device without chardev\n");
        exit(1);
    }

    qemu_chr_add_handlers(s->chr, chardev_can_rx, chardev_rx, chardev_event, s);
    qemu_chr_fe_open(s->chr);

//    sysbus_init_irq(dev, &s->irq);

   memory_region_init_io(&s->mmio, &mmio_ops, s, "avatar", s->mmio_size);
   sysbus_init_mmio_region(dev, &s->mmio);
   memory_region_add_subregion(get_system_memory(),
                                    s->mmio_address,
                                    &s->mmio);


    return 0;
}

static const VMStateDescription vmstate_avatar_regs = {
    .name = "avatar",
    .version_id = 1,
    .minimum_version_id = 0,
    .minimum_version_id_old = 0,
//    .post_load = scoop_post_load,
    .fields = (VMStateField []) {
//        VMSTATE_UINT16(status, ScoopInfo),
//        VMSTATE_UINT16(power, ScoopInfo),
//        VMSTATE_UINT32(gpio_level, ScoopInfo),
//        VMSTATE_UINT32(gpio_dir, ScoopInfo),
//        VMSTATE_UINT32(prev_level, ScoopInfo),
//        VMSTATE_UINT16(mcr, ScoopInfo),
//        VMSTATE_UINT16(cdr, ScoopInfo),
//        VMSTATE_UINT16(ccr, ScoopInfo),
//        VMSTATE_UINT16(irr, ScoopInfo),
//        VMSTATE_UINT16(imr, ScoopInfo),
//        VMSTATE_UINT16(isr, ScoopInfo),
//        VMSTATE_UNUSED_TEST(is_version_0, 2),
        VMSTATE_END_OF_LIST(),
    },
};

static SysBusDeviceInfo avatar_sysbus_info = {
    .init           = avatar_device_init,
    .qdev.name      = "avatar",
    .qdev.desc      = "Avatar remote device",
    .qdev.size      = sizeof(AvatarState),
    .qdev.vmsd      = &vmstate_avatar_regs,
    .qdev.props     = (Property[]) {
        DEFINE_PROP_UINT32("mmio_size", AvatarState, mmio_size, 0x100), 
        DEFINE_PROP_UINT32("mmio_address", AvatarState, mmio_address, 0), 
        DEFINE_PROP_CHR("chardev", AvatarState, chr),
        DEFINE_PROP_END_OF_LIST(),
    }
};

static void avatar_device_register(void)
{
    sysbus_register_withprop(&avatar_sysbus_info);
}

device_init(avatar_device_register)
