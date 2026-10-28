#include "tests/test_proc.h"
#include "uapi/syscall_nr.h"
#include "proc/process.h"
#include "ipc/pipe.h"
#include "ipc/msgq.h"
#include "exec/elf.h"
#include "sched/sched.h"
#include "time/clock.h"
#include "mm/pmm.h"
#include "sync/atomic.h"
#include "core/panic.h"
#include "lib/kprintf.h"

namespace {

const uint64_t STREAM_BYTES = 5 * PIPE_SIZE + 123;  // destul cat scriitorul sa se blocheze de mai multe ori
const uint32_t MSG_SIZE = 64;
const uint32_t MSG_DEPTH = 8;
const uint32_t MSG_COUNT = 2000;

Atomic<int> finished(0);
File* stream_rd = nullptr;
File* stream_wr = nullptr;
uint64_t stream_errors = 0;
MsgQueue* queue = nullptr;
uint64_t msg_errors = 0;

// Task 0 (idle) nu are voie sa se blocheze, deci asteapta cu yield + hlt
void wait_finished(int n){
    while(finished.load() < n){
        yield();
        __asm__ volatile("hlt");
    }
    finished.store(0);
}

void wait_tasks(uint64_t count){
    while(task_count() > count){
        yield();
        __asm__ volatile("hlt");
    }
}

void start(const char* name, TaskFn fn){
    Task* t = task_create_kernel(name, fn, nullptr);
    if(!t){
        panic("test_proc: task_create_kernel(%s) a esuat", name);
    }
    sched_add(t);
}

uint8_t pattern(uint64_t i){
    return (uint8_t)(i * 7 + (i >> 8));
}

void expect(int64_t got, int64_t want, const char* what){
    if(got != want){
        panic("test_proc: %s = %ld, asteptat %ld", what, got, want);
    }
}

// Scrie in bucati de marimi diferite; se blocheaza cand pipe-ul e plin
void stream_writer(void*){
    uint8_t chunk[700];
    for(uint64_t done = 0; done < STREAM_BYTES;){
        uint64_t n = STREAM_BYTES - done < sizeof(chunk) ? STREAM_BYTES - done : sizeof(chunk);
        for(uint64_t i = 0; i < n; i++){
            chunk[i] = pattern(done + i);
        }
        int64_t w = stream_wr->ops->write(stream_wr, chunk, n, false);
        if(w != (int64_t)n){
            stream_errors++;
            break;
        }
        done += n;
    }
    file_put(stream_wr);                        // cititorul vede EOF
    finished.fetch_add(1);
}

void stream_reader(void*){
    uint8_t chunk[333];
    uint64_t total = 0;
    for(;;){
        int64_t n = stream_rd->ops->read(stream_rd, chunk, sizeof(chunk), false);
        if(n <= 0){
            if(n < 0){
                stream_errors++;
            }
            break;
        }
        for(int64_t i = 0; i < n; i++){
            if(chunk[i] != pattern(total + i)){
                stream_errors++;
            }
        }
        total += n;
    }
    if(total != STREAM_BYTES){
        stream_errors++;
    }
    file_put(stream_rd);
    finished.fetch_add(1);
}

void test_pipe(){
    File* rd;
    File* wr;
    expect(pipe_create(rd, wr), 0, "pipe_create");
    uint8_t buf[PIPE_SIZE];
    expect(rd->ops->read(rd, buf, 1, true), -EAGAIN, "read pe pipe gol");
    expect(wr->ops->write(wr, buf, PIPE_SIZE, true), PIPE_SIZE, "write 4 KiB");
    expect(wr->ops->write(wr, buf, 1, true), -EAGAIN, "write pe pipe plin");
    expect(rd->ops->read(rd, buf, 100, true), 100, "read 100");
    expect(wr->ops->write(wr, buf, 200, true), 100, "write partial");
    expect(rd->ops->read(rd, buf, sizeof(buf), true), PIPE_SIZE, "read tot");
    expect(rd->ops->write(rd, buf, 1, true), -EBADF, "write pe capatul de citire");
    file_put(wr);
    expect(rd->ops->read(rd, buf, 1, false), 0, "EOF dupa inchiderea scrierii");
    file_put(rd);

    expect(pipe_create(rd, wr), 0, "pipe_create");
    file_put(rd);
    expect(wr->ops->write(wr, buf, 1, false), -EPIPE, "write fara cititori");
    file_put(wr);

    // Doua thread-uri, scriitorul se blocheaza cand pipe-ul e plin, cititorul cand e gol
    expect(pipe_create(stream_rd, stream_wr), 0, "pipe_create");
    start("pipe-rd", stream_reader);
    start("pipe-wr", stream_writer);
    wait_finished(2);
    if(stream_errors){
        panic("test_proc: %lu erori pe fluxul prin pipe", stream_errors);
    }
    kprintf("[OK] test_pipe (4 KiB, EAGAIN/EOF/EPIPE, %lu bytes intre doua thread-uri cu blocare)\n", STREAM_BYTES);
}

struct Msg {
    uint32_t seq;
    uint8_t fill[MSG_SIZE - 4];
};

void msg_producer(void*){
    Msg m;
    for(uint32_t i = 0; i < MSG_COUNT; i++){
        m.seq = i;
        uint32_t len = 4 + i % (MSG_SIZE - 4);
        for(uint32_t j = 0; j < len - 4; j++){
            m.fill[j] = (uint8_t)(i + j);
        }
        if(msgq_send(queue, &m, len, true) != 0){
            msg_errors++;
        }
    }
    finished.fetch_add(1);
}

void msg_consumer(void*){
    Msg m;
    for(uint32_t i = 0; i < MSG_COUNT; i++){
        int len = msgq_recv(queue, &m, sizeof(m), true);
        if(len != (int)(4 + i % (MSG_SIZE - 4)) || m.seq != i){
            msg_errors++;
            continue;
        }
        for(int j = 0; j < len - 4; j++){
            if(m.fill[j] != (uint8_t)(i + j)){
                msg_errors++;
            }
        }
    }
    finished.fetch_add(1);
}

void test_msgq(){
    expect(msgq_create(0, 4) != nullptr, 0, "msgq_create(0, 4)");
    queue = msgq_create(MSG_SIZE, MSG_DEPTH);
    if(!queue){
        panic("test_proc: msgq_create");
    }
    Msg m = {};
    expect(msgq_recv(queue, &m, sizeof(m), false), -EAGAIN, "recv pe coada goala");
    expect(msgq_send(queue, &m, MSG_SIZE + 1, false), -EINVAL, "mesaj prea mare");

    // Coada plina, apoi golita in ordine; masuram cel mai lung send/recv fara blocare
    uint64_t free_before = pmm_free_count();
    uint64_t worst = 0;
    for(int round = 0; round < 100; round++){
        for(uint32_t i = 0; i < MSG_DEPTH; i++){
            m.seq = i;
            uint64_t t0 = monotonic_ns();
            expect(msgq_send(queue, &m, MSG_SIZE, false), 0, "send");
            uint64_t dt = monotonic_ns() - t0;
            worst = dt > worst ? dt : worst;
        }
        expect(msgq_send(queue, &m, 4, false), -EAGAIN, "send pe coada plina");
        expect(msgq_count(queue), MSG_DEPTH, "msgq_count");
        expect(msgq_recv(queue, &m, 3, false), -EINVAL, "recv cu buffer prea mic");
        for(uint32_t i = 0; i < MSG_DEPTH; i++){
            uint64_t t0 = monotonic_ns();
            expect(msgq_recv(queue, &m, sizeof(m), false), MSG_SIZE, "recv");
            uint64_t dt = monotonic_ns() - t0;
            worst = dt > worst ? dt : worst;
            expect(m.seq, i, "ordinea FIFO");
        }
    }
    if(pmm_free_count() != free_before){
        panic("test_proc: msgq a alocat memorie in send/recv");
    }

    start("msg-cons", msg_consumer);
    start("msg-prod", msg_producer);
    wait_finished(2);
    if(msg_errors){
        panic("test_proc: %lu erori pe message queue", msg_errors);
    }
    msgq_destroy(queue);
    queue = nullptr;
    kprintf("[OK] test_msgq (%u x %u bytes, FIFO, EAGAIN, %u mesaje blocant intre doua thread-uri, "
            "send/recv max %lu ns)\n", MSG_DEPTH, MSG_SIZE, MSG_COUNT, worst);
}

int run_forktest(){
    static const char* const argv[] = {"forktest", nullptr};
    static const char* const envp[] = {"YOKAI=1", nullptr};
    uint64_t tasks_before = task_count();
    uint64_t pid = exec_spawn_module("forktest", argv, envp);
    if(!pid){
        panic("test_proc: forktest nu a pornit");
    }
    wait_tasks(tasks_before);
    int code;
    if(!process_exit_status(pid, code)){
        panic("test_proc: forktest (pid %lu) a disparut fara exit", pid);
    }
    return code;
}

void test_fork(){
    uint64_t procs_before = process_count();
    // Prima rulare creste cache-urile slab; a doua trebuie sa intoarca exact paginile
    int code = run_forktest();
    uint64_t free_before = pmm_free_count();
    if(code == 0){
        code = run_forktest();
    }
    if(code != 0){
        panic("test_proc: forktest a iesit cu %d (verificarea %d din forktest.S a esuat)", code, code);
    }
    if(process_count() != procs_before){
        panic("test_proc: %lu procese neeliberate", process_count() - procs_before);
    }
    if(pmm_free_count() != free_before){
        panic("test_proc: %ld pagini pierdute", (int64_t)(free_before - pmm_free_count()));
    }
}

}

void test_proc_run(){
    __asm__ volatile("sti");
    test_pipe();
    test_msgq();
    test_fork();
    __asm__ volatile("cli");
    kprintf("[OK] test_proc (fork + pipe + waitpid, execve, WNOHANG, orfan; fara procese sau pagini pierdute)\n");
}
