#include <apps_commons.h>
#include <app_helpers.h>
#include <page.h>
#include <iostream>
#include <hs_clock.h>
#include <sys/mman.h>
#include <experimental/filesystem>

// do we need it ?
DEFINE_uint64(alloc_size, 0, "Alloc size for each request, unit is MB");
DEFINE_string(latency_file, "latency.txt", "Latency file name");
DEFINE_string(bandwidth_file, "bandwidth.txt", "Bandwidth file name");
DEFINE_string(cxl_fake_folder, "", "Mount tmpfs on this folder, which use another numa memory to fake cxl memory");
DEFINE_bool(no_cow, false, "Don't use cow");

double total_speed = 0;
std::string folder_name;
hdr_histogram *latency_hist_;

static size_t file_num;

void test_fork(AppContext *c, size_t *raddr)
{
    std::random_device rd;
    std::uniform_int_distribution<int> dist(0, RAND_MAX);

    size_t write_num = FLAGS_block_size / 4096;
    size_t max_num = FLAGS_alloc_size / sizeof(size_t);

    std::vector<rmem::Timer> timers(FLAGS_concurrency);
    if (FLAGS_concurrency == 1)
    {
        rmem::Timer now_clock;
        std::vector<std::vector<int>> now_nums(FLAGS_test_loop, std::vector<int>(write_num));
        for (size_t i = 0; i < FLAGS_test_loop; i++)
        {
            for (size_t j = 0; j < write_num; j++)
            {
                now_nums[i][j] = dist(rd) % max_num;
            }
        }
        std::vector<size_t> rand_file_nums(FLAGS_test_loop);
        for (size_t i = 0; i < FLAGS_test_loop; i++)
        {
            rand_file_nums[i] = dist(rd) % file_num;
        }
        std::cout << "begin thread " << c->thread_id_ << std::endl;
        now_clock.tic();
        size_t count = 0;
        for (size_t i = 0; i < FLAGS_test_loop; i++)
        {

            timers[0].tic();

            // int now_num = 0;

            for (size_t t = 0; t < write_num; t++)
            {
                int now_num = now_nums[i][t];
                size_t tmp = raddr[now_num];
                raddr[now_num] = tmp + 1;
            }

            if (FLAGS_no_cow)
            {
                std::string src_file = folder_name + "cxlspeed_" + std::to_string(c->thread_id_) + "_" + std::to_string(rand_file_nums[i]);

                std::string dst_file = folder_name + "cxlspeedcp_" + std::to_string(c->thread_id_) + "_" + std::to_string(rand_file_nums[i]);

                try // If you want to avoid exception handling, then use the error code overload of the following functions.
                {
                    std::experimental::filesystem::copy_file(src_file, dst_file, std::experimental::filesystem::copy_options::overwrite_existing);
                }
                catch (std::exception &e) // Not using fs::filesystem_error since std::bad_alloc can throw too.
                {
                    std::cout << e.what();
                }
            }

            hdr_record_value_atomic(latency_hist_,
                                    static_cast<int64_t>(timers[0].toc() * 10));
            count++;
            if (ctrl_c_pressed == 1)
            {
                break;
            }
        }
        total_speed += (double)count * 1e6 / now_clock.toc();
        std::cout << "end thread " << c->thread_id_ << std::endl;
    }
}

bool write_bandwidth(const std::string &filename)
{
    FILE *fp = fopen(filename.c_str(), "w");
    if (fp == nullptr)
    {
        return false;
    }
    fprintf(fp, "%f\n", total_speed);
    fclose(fp);
    return true;
}

bool write_latency_and_reset(const std::string &filename)
{

    FILE *fp = fopen(filename.c_str(), "w");
    if (fp == nullptr)
    {
        return false;
    }
    hdr_percentiles_print(latency_hist_, fp, 5, 10, CLASSIC);
    fclose(fp);
    hdr_reset(latency_hist_);
    return true;
}

void client_func(size_t thread_id)
{
    AppContext c;
    c.thread_id_ = thread_id;
    char buf[4096] = "123456789";

    for (size_t i = 0; i < file_num; i++)
    {
        std::string filename = folder_name + "cxlspeed_" + std::to_string(thread_id) + "_" + std::to_string(i);
        std::ofstream file(filename, std::ios::out | std::ios::binary);
        if (!file.is_open())
        {
            std::cout << "open file fail" << std::endl;
            exit(1);
        }
        for (size_t j = 0; j < FLAGS_block_size / 4096; j++)
        {
            file.write(buf, 4096);
        }
        file.close();
    }

    std::string filename = folder_name + "cxlpt_" + std::to_string(thread_id);
    std::ofstream file(filename, std::ios::out | std::ios::binary);
    if (!file.is_open())
    {
        std::cout << "open file fail" << std::endl;
        exit(1);
    }
    for (size_t j = 0; j < FLAGS_alloc_size / 4096; j++)
    {
        file.write(buf, 4096);
    }
    file.close();

    sleep(2);

    FILE *c_file = fopen(filename.c_str(), "r+");
    void *addr = mmap(NULL, FLAGS_alloc_size, PROT_READ | PROT_WRITE, MAP_SHARED, fileno(c_file), 0);
    rmem::rt_assert(addr != MAP_FAILED, "mmap failed");

    test_fork(&c, static_cast<size_t *>(addr));
}

int main(int argc, char **argv)
{
    signal(SIGINT, ctrl_c_handler);
    gflags::ParseCommandLineFlags(&argc, &argv, true);
    // check_common_gflags();
    rmem::rt_assert(kAppMaxConcurrency >= FLAGS_concurrency, "kAppMaxConcurrency must be >= FLAGS_concurrency");
    rmem::rt_assert(FLAGS_alloc_size != 0, "alloc_size must be set");
    rmem::rt_assert(rmem::AsyncReceivedReqSize >= FLAGS_concurrency, "AsyncReceivedReqSize must be >= FLAGS_concurrency");

    FLAGS_alloc_size = MB(FLAGS_alloc_size);
    std::cout << getpid() << std::endl;

    folder_name = FLAGS_cxl_fake_folder;
    if (folder_name[folder_name.size() - 1] != '/')
    {
        folder_name += '/';
    }

    std::vector<std::thread> threads(FLAGS_client_thread_num);

    FLAGS_concurrency = 1;

    int ret = hdr_init(1, 1000 * 1000 * 10, 3,
                       &latency_hist_);
    rmem::rt_assert(ret == 0, "hdr_init failed");

    file_num = (128 << 20) / FLAGS_block_size;

    for (size_t i = 0; i < FLAGS_client_thread_num; i++)
    {
        threads[i] = std::thread(client_func, i);
        rmem::bind_to_core(threads[i], FLAGS_numa_node_user_thread, i);
    }

    for (auto &t : threads)
    {
        t.join();
    }

    write_bandwidth(FLAGS_bandwidth_file);
    write_latency_and_reset(FLAGS_latency_file);
    hdr_close(latency_hist_);
}