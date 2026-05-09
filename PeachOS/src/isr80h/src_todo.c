/* src_todo.c - kernel-side implementation of the todo subsystem.

   this is the bulk of the project's code. five syscall entry points
   (cmds 10..14) plus the helpers they need:

     cmd 10  add      - copy description from user, find a free slot,
                        assign a new id
     cmd 11  list     - print active tasks straight from the kernel
     cmd 12  remove   - find by id, zero out the slot
     cmd 13  save     - serialize tasks, XOR with a user key, write to
                        a fixed disk region
     cmd 14  load     - read back, decrypt, validate magic, restore

   design notes worth defending in class:

   - tasks live in a fixed-size static array (TODO_MAX_TASKS = 64).
     no kernel heap involvement, no fragmentation, and removal is
     just clearing a slot - O(1) and trivially safe.

   - we use an "active" flag rather than compacting the array, so ids
     stay stable across removes.

   - todo_next_id is monotonic. ids are never recycled, even after
     the slot is cleared, so a stale id can't quietly hit a brand-new
     task.

   - copy_string_from_task is used everywhere we get a pointer from
     userspace. we never deref a raw user pointer in the kernel - a
     bad pointer should fail the syscall, not crash the kernel.

   - save/load uses a tiny on-disk layout: one catalog sector + N
     payload sectors per save slot, both stamped with a magic string
     so load can refuse to interpret garbage. the XOR pass over the
     payload is *not* real encryption - it's there to keep a hex dump
     of the disk image from immediately leaking task text and to make
     sure a corrupted read fails the magic check loudly. */

#include "src_todo.h"
#include "task/task.h"
#include "string/string.h"
#include "memory/memory.h"
#include "disk/disk.h"
#include "status.h"
#include "kernel.h"

/* sizing constants. picked small enough that the static state is cheap
   and big enough that a demo never bumps the limits. */
#define TODO_MAX_TASKS 64
#define TODO_DESC_MAX 64
#define TODO_FILENAME_MAX 32
#define TODO_KEY_MAX 64

/* on-disk save area constants. the catalog lives at a fixed LBA way
   past anything the FAT image touches, so we can't accidentally
   clobber a real user file. each save slot is 10 sectors (5 KB),
   plenty for 64 serialized tasks. */
#define TODO_SAVE_SLOTS 8
#define TODO_SLOT_SECTORS 10
#define TODO_SLOT_BYTES (TODO_SLOT_SECTORS * 512)
#define TODO_CATALOG_LBA 32000
#define TODO_DATA_BASE_LBA (TODO_CATALOG_LBA + 1)

struct todo_task
{
    int id;
    int active;     /* 1 if this slot is in use, 0 if free */
    int complete;   /* not exposed via the REPL yet, but kept in the
                       struct + on-disk format so adding a "done"
                       command later is a one-line change */
    char description[TODO_DESC_MAX];
};

/* catalog block: one sector at TODO_CATALOG_LBA. tracks which save
   slots are in use, the saved payload length per slot, and the
   user-supplied filename for each. */
struct todo_catalog
{
    char magic[8];
    int version;
    unsigned char used[TODO_SAVE_SLOTS];
    unsigned int lengths[TODO_SAVE_SLOTS];
    char names[TODO_SAVE_SLOTS][TODO_FILENAME_MAX];
};

/* on-disk payload header, one per save slot. */
struct todo_serial_header
{
    char magic[8];
    int next_id;
    int task_count;
};

/* one of these per active task in the serialized payload. */
struct todo_serial_entry
{
    int id;
    int complete;
    char description[TODO_DESC_MAX];
};

/* the entire in-memory model. static so it survives across syscalls
   and lives in BSS - no allocation paths to worry about. */
static struct todo_task todo_tasks[TODO_MAX_TASKS];
static int todo_next_id = 1;

/* tiny int-to-string + print, because we don't have printf in here.
   builds the digits backwards into a stack buffer then prints from the
   first non-zero position. handles negatives even though task ids are
   never negative - cheap insurance. */
static void todo_print_int(int value)
{
    char out[16];
    int pos = 15;
    int negative = 0;
    out[15] = 0;

    if (value == 0)
    {
        print("0");
        return;
    }

    if (value < 0)
    {
        negative = 1;
        value = -value;
    }

    while(value > 0 && pos > 0)
    {
        int digit = value % 10;
        out[--pos] = '0' + digit;
        value /= 10;
    }

    if (negative && pos > 0)
    {
        out[--pos] = '-';
    }

    print(&out[pos]);
}

/* find first free slot. returns -1 if the table is full. linear scan
   is fine - 64 entries is nothing. */
static int todo_find_free_slot()
{
    for (int i = 0; i < TODO_MAX_TASKS; i++)
    {
        if (!todo_tasks[i].active)
        {
            return i;
        }
    }

    return -1;
}

/* find an active task by its public id. */
static int todo_find_task_by_id(int id)
{
    for (int i = 0; i < TODO_MAX_TASKS; i++)
    {
        if (todo_tasks[i].active && todo_tasks[i].id == id)
        {
            return i;
        }
    }

    return -1;
}

static int todo_task_count()
{
    int count = 0;
    for (int i = 0; i < TODO_MAX_TASKS; i++)
    {
        if (todo_tasks[i].active)
        {
            count++;
        }
    }

    return count;
}

/* XOR pass over `data`. used for both encrypt and decrypt - same op.
   not real crypto. see the file header comment for why we still bother. */
static void todo_xor_crypt(char* data, int len, const char* key, int key_len)
{
    for (int i = 0; i < len; i++)
    {
        data[i] = data[i] ^ key[i % key_len];
    }
}

/* wipe a catalog struct back to a fresh, empty-but-valid state. used
   when the on-disk catalog is missing or corrupt. */
static void todo_catalog_default(struct todo_catalog* catalog)
{
    memset(catalog, 0, sizeof(struct todo_catalog));
    strncpy(catalog->magic, "TDOCAT1", sizeof(catalog->magic));
    catalog->version = 1;
}

/* read the catalog sector. if the magic/version don't check out we
   *don't* fail - we hand back a fresh empty catalog. that way the
   first ever save on a clean disk just works. */
static int todo_catalog_load(struct todo_catalog* catalog)
{
    struct disk* disk = disk_get(0);
    if (!disk)
    {
        return -EIO;
    }

    char sector[512];
    int res = disk_read_block(disk, TODO_CATALOG_LBA, 1, sector);
    if (res < 0)
    {
        return res;
    }

    memcpy(catalog, sector, sizeof(struct todo_catalog));
    if (strncmp(catalog->magic, "TDOCAT1", 7) != 0 || catalog->version != 1)
    {
        todo_catalog_default(catalog);
    }

    return 0;
}

static int todo_catalog_save(struct todo_catalog* catalog)
{
    struct disk* disk = disk_get(0);
    if (!disk)
    {
        return -EIO;
    }

    char sector[512];
    memset(sector, 0, sizeof(sector));
    memcpy(sector, catalog, sizeof(struct todo_catalog));
    return disk_write_block(disk, TODO_CATALOG_LBA, 1, sector);
}

/* find a slot to save into: if a slot already has this filename, reuse
   it (so save acts as overwrite). otherwise hand back the first free
   slot. -1 only when both the named slot doesn't exist AND no slots
   are free. */
static int todo_save_slot_index(struct todo_catalog* catalog, const char* filename)
{
    for (int i = 0; i < TODO_SAVE_SLOTS; i++)
    {
        if (catalog->used[i] && strncmp(catalog->names[i], filename, TODO_FILENAME_MAX) == 0)
        {
            return i;
        }
    }

    for (int i = 0; i < TODO_SAVE_SLOTS; i++)
    {
        if (!catalog->used[i])
        {
            return i;
        }
    }

    return -1;
}

/* strict lookup for load: only returns a slot if the name actually
   matches an existing entry. */
static int todo_find_slot_index(struct todo_catalog* catalog, const char* filename)
{
    for (int i = 0; i < TODO_SAVE_SLOTS; i++)
    {
        if (catalog->used[i] && strncmp(catalog->names[i], filename, TODO_FILENAME_MAX) == 0)
        {
            return i;
        }
    }

    return -1;
}



/* rebuild the in-memory task array from a decrypted payload buffer.
   validates the magic before touching todo_tasks - that's the bit
   that turned out to be a real lifesaver during development. without
   it, a wrong key just silently restored garbage. */
static int todo_restore_from_plain(char* plain, unsigned int plain_len)
{
    struct todo_serial_header header;
    memset(&header, 0, sizeof(header));
    if ((int) sizeof(header) > (int) plain_len)
    {
        return -EINVARG;
    }

    memcpy(&header, plain, sizeof(header));
    if (strncmp(header.magic, "TDODAT1", 7) != 0)
    {
        return -EINVARG;
    }

    memset(todo_tasks, 0, sizeof(todo_tasks));
    todo_next_id = header.next_id;

    int offset = sizeof(header);
    for (int i = 0; i < header.task_count && i < TODO_MAX_TASKS; i++)
    {
        if (offset + (int) sizeof(struct todo_serial_entry) > (int) plain_len)
        {
            break;
        }

        struct todo_serial_entry entry;
        memcpy(&entry, plain + offset, sizeof(entry));
        offset += sizeof(entry);

        todo_tasks[i].id = entry.id;
        todo_tasks[i].active = 1;
        todo_tasks[i].complete = entry.complete;
        strncpy(todo_tasks[i].description, entry.description, sizeof(todo_tasks[i].description));
    }

    if (todo_next_id <= 0)
    {
        todo_next_id = 1;
    }

    return 0;
}

static int todo_save_to_disk(const char* filename, const char* key)
{
    int key_len = strnlen(key, TODO_KEY_MAX);
    if (key_len <= 0)
    {
        return -EINVARG;
    }

    char plain[TODO_SLOT_BYTES];
    char enc[TODO_SLOT_BYTES];
    memset(plain, 0, sizeof(plain));
    memset(enc, 0, sizeof(enc));

    struct todo_serial_header header;
    memset(&header, 0, sizeof(header));
    strncpy(header.magic, "TDODAT1", sizeof(header.magic));
    header.next_id = todo_next_id;
    header.task_count = todo_task_count();

    int offset = 0;
    memcpy(plain + offset, &header, sizeof(header));
    offset += sizeof(header);

    for (int i = 0; i < TODO_MAX_TASKS; i++)
    {
        if (!todo_tasks[i].active)
        {
            continue;
        }

        struct todo_serial_entry entry;
        memset(&entry, 0, sizeof(entry));
        entry.id = todo_tasks[i].id;
        entry.complete = todo_tasks[i].complete;
        strncpy(entry.description, todo_tasks[i].description, sizeof(entry.description));

        if (offset + (int) sizeof(entry) > TODO_SLOT_BYTES)
        {
            return -ENOMEM;
        }

        memcpy(plain + offset, &entry, sizeof(entry));
        offset += sizeof(entry);
    }

    memcpy(enc, plain, sizeof(plain));
    todo_xor_crypt(enc, offset, key, key_len);

    struct todo_catalog catalog;
    int res = todo_catalog_load(&catalog);
    if (res < 0)
    {
        return res;  /* catalog load failed */
    }

    int slot = todo_save_slot_index(&catalog, filename);
    if (slot < 0)
    {
        return -ENOMEM;  /* no save slots available */
    }

    struct disk* disk = disk_get(0);
    if (!disk)
    {
        return -EINVARG;  /* disk not found */
    }

    unsigned int lba = TODO_DATA_BASE_LBA + (slot * TODO_SLOT_SECTORS);
    for (int i = 0; i < TODO_SLOT_SECTORS; i++)
    {
        res = disk_write_block(disk, lba + i, 1, enc + (i * 512));
        if (res < 0)
        {
            return res;  /* disk write failed */
        }
    }

    catalog.used[slot] = 1;
    catalog.lengths[slot] = offset;
    strncpy(catalog.names[slot], filename, TODO_FILENAME_MAX);

    res = todo_catalog_save(&catalog);
    if (res < 0)
    {
        return res;  /* catalog save failed */
    }

    return 0;
}

static int todo_load_from_disk(const char* filename, const char* key)
{
    int key_len = strnlen(key, TODO_KEY_MAX);
    if (key_len <= 0)
    {
        return -EINVARG;
    }

    char enc[TODO_SLOT_BYTES];
    char plain[TODO_SLOT_BYTES];
    memset(enc, 0, sizeof(enc));
    memset(plain, 0, sizeof(plain));

    unsigned int data_len = 0;
    int res = 0;

    struct todo_catalog catalog;
    res = todo_catalog_load(&catalog);
    if (res >= 0)
    {
        int slot = todo_find_slot_index(&catalog, filename);
        if (slot >= 0 && catalog.lengths[slot] > 0 && catalog.lengths[slot] <= TODO_SLOT_BYTES)
        {
            struct disk* disk = disk_get(0);
            if (disk)
            {
                unsigned int lba = TODO_DATA_BASE_LBA + (slot * TODO_SLOT_SECTORS);
                res = 0;
                for (int i = 0; i < TODO_SLOT_SECTORS; i++)
                {
                    res = disk_read_block(disk, lba + i, 1, enc + (i * 512));
                    if (res < 0)
                    {
                        break;
                    }
                }

                if (res >= 0)
                {
                    data_len = catalog.lengths[slot];
                    memcpy(plain, enc, sizeof(enc));
                    todo_xor_crypt(plain, data_len, key, key_len);
                    res = todo_restore_from_plain(plain, data_len);
                    if (res >= 0)
                    {
                        return 0;
                    }
                }
            }
        }
    }

    return -EINVARG;  /* file not found on disk */
}

void* isr80h_command10_todo_add(struct interrupt_frame* frame)
{
    /* user passed a pointer to the description string. copy it into a
       kernel buffer rather than dereferencing it in place - that's the
       only safe way to handle a foreign pointer in here. */
    void* task_ptr = task_get_stack_item(task_current(), 0);
    char task_desc[TODO_DESC_MAX];
    int res = copy_string_from_task(task_current(), task_ptr, task_desc, sizeof(task_desc));
    if (res < 0)
    {
        return ERROR(-EINVARG);
    }

    /* reject empty strings - they'd just clutter the list. */
    if (strnlen(task_desc, sizeof(task_desc)) <= 0)
    {
        return ERROR(-EINVARG);
    }

    int slot = todo_find_free_slot();
    if (slot < 0)
    {
        return ERROR(-ENOMEM);
    }

    todo_tasks[slot].id = todo_next_id++;
    todo_tasks[slot].active = 1;
    todo_tasks[slot].complete = 0;
    strncpy(todo_tasks[slot].description, task_desc, sizeof(todo_tasks[slot].description));

    /* hand the new id back to user space as the syscall return value. */
    return (void*) todo_tasks[slot].id;
}

void* isr80h_command11_todo_list(struct interrupt_frame* frame)
{
    (void) frame;

    int count = todo_task_count();
    if (count == 0)
    {
        print("No tasks available\n");
        return 0;
    }

    for (int i = 0; i < TODO_MAX_TASKS; i++)
    {
        if (!todo_tasks[i].active)
        {
            continue;
        }

        print("[");
        todo_print_int(todo_tasks[i].id);
        print("] ");
        print(todo_tasks[i].description);
        print("\n");
    }

    return 0;
}

void* isr80h_command12_todo_remove(struct interrupt_frame* frame)
{
    (void) frame;

    int id = (int) task_get_stack_item(task_current(), 0);
    int slot = todo_find_task_by_id(id);
    if (slot < 0)
    {
        return ERROR(-EINVARG);
    }

    /* memset the entire struct rather than just clearing `active`, so
       there's no chance stale description bytes leak into a later
       slot reuse. cheap belt-and-braces move. */
    memset(&todo_tasks[slot], 0, sizeof(struct todo_task));
    return 0;
}

void* isr80h_command13_todo_save(struct interrupt_frame* frame)
{
    (void) frame;

    void* filename_ptr = task_get_stack_item(task_current(), 0);
    void* key_ptr = task_get_stack_item(task_current(), 1);

    char filename[TODO_FILENAME_MAX];
    char key[TODO_KEY_MAX];

    int res = copy_string_from_task(task_current(), filename_ptr, filename, sizeof(filename));
    if (res < 0)
    {
        return ERROR(-EINVARG);
    }

    res = copy_string_from_task(task_current(), key_ptr, key, sizeof(key));
    if (res < 0)
    {
        return ERROR(-EINVARG);
    }

    if (strnlen(filename, sizeof(filename)) <= 0)
    {
        return ERROR(-EINVARG);
    }

    res = todo_save_to_disk(filename, key);
    if (res < 0)
    {
        return ERROR(res);
    }

    return 0;
}

void* isr80h_command14_todo_load(struct interrupt_frame* frame)
{
    (void) frame;

    void* filename_ptr = task_get_stack_item(task_current(), 0);
    void* key_ptr = task_get_stack_item(task_current(), 1);

    char filename[TODO_FILENAME_MAX];
    char key[TODO_KEY_MAX];

    int res = copy_string_from_task(task_current(), filename_ptr, filename, sizeof(filename));
    if (res < 0)
    {
        return ERROR(-EINVARG);
    }

    res = copy_string_from_task(task_current(), key_ptr, key, sizeof(key));
    if (res < 0)
    {
        return ERROR(-EINVARG);
    }

    if (strnlen(filename, sizeof(filename)) <= 0)
    {
        return ERROR(-EINVARG);
    }

    res = todo_load_from_disk(filename, key);
    if (res < 0)
    {
        return ERROR(res);
    }

    return 0;
}
