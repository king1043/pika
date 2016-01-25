#include "pika_define.h"
#include "pika_util.h"
#include "util.h"
#include "pika_epoll.h"
#include "pika_item.h"
#include "pika_thread.h"
#include "pika_conf.h"
#include "pika_conn.h"
#include "mutexlock.h"
#include "pika_server.h"
#include "mario_handler.h"
#include "nemo_backupable.h"

#include <glog/logging.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <poll.h>
#include <fstream>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <signal.h>
#include <set>

extern PikaConf *g_pikaConf;
extern mario::Mario *g_pikaMario;

static void save_time_tm(struct tm *dst_ptr, struct tm *src_ptr)
{
    assert(dst_ptr != NULL);
    assert(src_ptr != NULL);
    dst_ptr->tm_sec  = src_ptr->tm_sec;
    dst_ptr->tm_min  = src_ptr->tm_min;
    dst_ptr->tm_hour = src_ptr->tm_hour;
    dst_ptr->tm_mday = src_ptr->tm_mday;
    dst_ptr->tm_mon  = src_ptr->tm_mon;
    dst_ptr->tm_year = src_ptr->tm_year;
    dst_ptr->tm_wday = src_ptr->tm_wday;
    dst_ptr->tm_yday = src_ptr->tm_yday;
    dst_ptr->tm_isdst = src_ptr->tm_isdst;
}

Status PikaServer::SetBlockType(BlockType type)
{
    Status s;
    if ((flags_ = fcntl(sockfd_, F_GETFL, 0)) < 0) {
        s = Status::Corruption("F_GETFEL error");
        close(sockfd_);
        return s;
    }
    if (type == kBlock) {
        flags_ &= (~O_NONBLOCK);
    } else if (type == kNonBlock) {
        flags_ |= O_NONBLOCK;
    }
    if (fcntl(sockfd_, F_SETFL, flags_) < 0) {
        s = Status::Corruption("F_SETFL error");
        close(sockfd_);
        return s;
    }

    if ((flags_ = fcntl(slave_sockfd_, F_GETFL, 0)) < 0) {
        s = Status::Corruption("F_GETFEL error");
        close(slave_sockfd_);
        return s;
    }
    if (type == kBlock) {
        flags_ &= (~O_NONBLOCK);
    } else if (type == kNonBlock) {
        flags_ |= O_NONBLOCK;
    }
    if (fcntl(slave_sockfd_, F_SETFL, flags_) < 0) {
        s = Status::Corruption("F_SETFL error");
        close(slave_sockfd_);
        return s;
    }
    return Status::OK();
}

PikaServer::PikaServer()
{
    // init statistics variables
    shutdown = false;
    worker_num = 0;

    pthread_rwlock_init(&rwlock_, NULL);

    nemo::Options option;
    option.write_buffer_size = g_pikaConf->write_buffer_size();
    option.target_file_size_base = g_pikaConf->target_file_size_base();

    LOG(WARNING) << "Prepare DB...";
    db_ = new nemo::Nemo(g_pikaConf->db_path(), option);
    LOG(WARNING) << "DB Success";
    // init sock
    sockfd_ = socket(AF_INET, SOCK_STREAM, 0);
    memset(&servaddr_, 0, sizeof(servaddr_));
    int yes = 1;
    if (setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
        LOG(FATAL) << "setsockopt SO_REUSEADDR: " << strerror(errno);
    }
    port_ = g_pikaConf->port();
    servaddr_.sin_family = AF_INET;
    servaddr_.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr_.sin_port = htons(port_);

    int ret = bind(sockfd_, (struct sockaddr *) &servaddr_, sizeof(servaddr_));
    if (ret < 0) {
        LOG(FATAL) << "bind error: "<< strerror(errno);
    }
    listen(sockfd_, 10);

    // init slave_sock
    slave_sockfd_ = socket(AF_INET, SOCK_STREAM, 0);
    memset(&servaddr_, 0, sizeof(servaddr_));
    yes = 1;
    if (setsockopt(slave_sockfd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
        LOG(FATAL) << "setsockopt SO_REUSEADDR: " << strerror(errno);
    }
    int slave_port = g_pikaConf->port() + 100;
    servaddr_.sin_family = AF_INET;
    servaddr_.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr_.sin_port = htons(slave_port);

    ret = bind(slave_sockfd_, (struct sockaddr *) &servaddr_, sizeof(servaddr_));
    if (ret < 0) {
        LOG(FATAL) << "bind error: "<< strerror(errno);
    }
    listen(slave_sockfd_, 10);

    SetBlockType(kNonBlock);

    // init pika epoll
    pikaEpoll_ = new PikaEpoll();
    pikaEpoll_->PikaAddEvent(sockfd_, EPOLLIN | EPOLLERR | EPOLLHUP);
    pikaEpoll_->PikaAddEvent(slave_sockfd_, EPOLLIN | EPOLLERR | EPOLLHUP);

    thread_num_ = g_pikaConf->thread_num() + g_pikaConf->slave_thread_num() + 1;
    last_thread_ = 0;
    last_slave_thread_ = 0;
    for (int i = 0; i < thread_num_; i++) {
        pikaThread_[i] = new PikaThread(i);
    }

    ms_state_ = PIKA_REP_SINGLE;
    repl_state_ = PIKA_SINGLE;
    flushing_ = false;
    dump_filenum_ = 0;
    dump_pro_offset_ = 0;
    bgsaving_ = false;
    purging_ = false;
    is_readonly_ = g_pikaConf->readonly();
    info_keyspacing_ = false;
    start_time_s_ = time(NULL);
    last_purge_time_s_ = start_time_s_;
    struct tm *time_tm_ptr = localtime(&start_time_s_);
    save_time_tm(&last_autopurge_time_tm_, time_tm_ptr);
    save_time_tm(&start_time_tm_, time_tm_ptr);
//    options_.create_if_missing = true;
//    options_.write_buffer_size = 1500000000;
//    leveldb::Status s = leveldb::DB::Open(options_, "/tmp/testdb", &db_);
//    leveldb::Status s = leveldb::DB::Open(options_, "/tmp/testdb", &db_);
//    db_ = new nemo::Nemo("/tmp/testdb");
//    if (!s.ok()) {
//        log_err("Open db failed");
//    }

    // start the pikaThread_ thread
    for (int i = 0; i < thread_num_; i++) {
        worker_num++;
        pthread_create(&(pikaThread_[i]->thread_id_), NULL, &(PikaServer::StartThread), pikaThread_[i]);
    }
    dump_thread_id_ = 0;
}

PikaServer::~PikaServer()
{
    for (int i = 0; i < thread_num_; i++) {
        delete(pikaThread_[i]);
    }
    delete(pikaEpoll_);
    close(sockfd_);
    pthread_rwlock_destroy(&rwlock_);

    delete db_;
}

std::string PikaServer::GetServerIp() {
    struct ifreq ifr;
    strcpy(ifr.ifr_name, "eth0");
    if (ioctl(sockfd_, SIOCGIFADDR, &ifr) !=  0) {
        LOG(FATAL) << "ioctl error";
    }
    return std::string(inet_ntoa(((struct sockaddr_in*)&(ifr.ifr_addr))->sin_addr));
}

int PikaServer::GetServerPort() {
    return port_;
}

bool PikaServer::LoadDb(std::string& path) {
    RWLock l(&rwlock_, true);
    nemo::Options option;
    option.write_buffer_size = g_pikaConf->write_buffer_size();
    LOG(WARNING) << "Prepare open new db...";
    nemo::Nemo *t_db = new nemo::Nemo(path, option);
    LOG(WARNING) << "open new db success";
    nemo::Nemo *t = db_;
    db_ = t_db;
    delete t;
    return true;
}

int is_dir(const char* filename) {
    struct stat buf;
    int ret = stat(filename,&buf);
    if (0 == ret) {
        if (buf.st_mode & S_IFDIR) {
            //folder
            return 0;
        } else {
            //file
            return 1;
        }
    }
    return -1;
}

int delete_dir(const char* dirname)
{
    char chBuf[256];
    DIR * dir = NULL;
    struct dirent *ptr;
    int ret = 0;
    dir = opendir(dirname);
    if (NULL == dir) {
        return -1;
    }
    while((ptr = readdir(dir)) != NULL) {
        ret = strcmp(ptr->d_name, ".");
        if (0 == ret) {
            continue;
        }
        ret = strcmp(ptr->d_name, "..");
        if (0 == ret) {
            continue;
        }
        snprintf(chBuf, 256, "%s/%s", dirname, ptr->d_name);
        ret = is_dir(chBuf);
        if (0 == ret) {
            //is dir
            ret = delete_dir(chBuf);
            if (0 != ret) {
                return -1;
            }
        }
        else if (1 == ret) {
            //is file
            ret = remove(chBuf);
            if(0 != ret) {
                return -1;
            }
        }
    }
    (void)closedir(dir);
    ret = remove(dirname);
    if (0 != ret) {
        return -1;
    }
    return 0;
}

bool PikaServer::Flushall() {
    MutexLock l(&mutex_);
    if (flushing_) {
        return false;
    }
    flushing_ = true;
    std::string dbpath = g_pikaConf->db_path();
    if (dbpath[dbpath.length() - 1] == '/') {
        dbpath.erase(dbpath.length() - 1);
    }
    int pos = dbpath.find_last_of('/');
    dbpath = dbpath.substr(0, pos);
    dbpath.append("/deleting");
    rename(g_pikaConf->db_path(), dbpath.c_str());

    RWLock wl(&rwlock_, true);
    LOG(WARNING) << "Delete old db...";
    delete db_;

    nemo::Options option;
    option.write_buffer_size = g_pikaConf->write_buffer_size();
    option.target_file_size_base = g_pikaConf->target_file_size_base();
    if (g_pikaConf->compression() == "none") {
        option.compression = false;
    }
    LOG(WARNING) << "Prepare open new db...";
    db_ = new nemo::Nemo(g_pikaConf->db_path(), option);
    LOG(WARNING) << "open new db success";
    flush_args *arg = new flush_args;
    arg->p = (void*)this;
    arg->path = dbpath;
    pthread_create(&flush_thread_id_, NULL, &(PikaServer::StartFlush), arg);
    return true;
}

void* PikaServer::StartFlush(void* arg) {
    PikaServer* p = (PikaServer*)(((flush_args*)arg)->p);
    std::string path = ((flush_args*)arg)->path;
    LOG(INFO) << "Deleting " << path;

    delete_dir(path.c_str());
    {
    MutexLock l(p->Mutex());
    p->flushing_ = false;
    }
    delete (flush_args*)arg;
    return NULL;
}

void PikaServer::ClearBackupEngine() {

    if (backup_engine_ != NULL)
        delete backup_engine_;
    backup_engine_ = NULL;  
}

std::string PikaServer::GetTmpDumpDir() {
    std::string dump_path(g_pikaConf->dump_path());
    if (dump_path[dump_path.length() - 1] != '/') {
        dump_path.append("/");
    }
    return dump_path.append("tmp/");
}

void PikaServer::Dump() {
    MutexLock l(&mutex_);
    if (bgsaving_) {
        return;
    }
    //Prepare for usefull dir
    bgsaving_start_time_ = time(NULL);
    strftime(dump_time_, sizeof(dump_time_), "%Y%m%d%H%M%S",localtime(&bgsaving_start_time_));
    std::string dump_path(g_pikaConf->dump_path());
    if (dump_path[dump_path.length() - 1] != '/') {
        dump_path.append("/");
    }
    dump_path.append(g_pikaConf->dump_prefix());
    dump_path.append(dump_time_, 8);
    if (is_dir(dump_path.c_str()) == 0 && delete_dir(dump_path.c_str()) != 0) {
        LOG(ERROR) << "remove exist dump dir failed";
        return;
    }
    mkpath(dump_path.c_str(), 0755);
    std::string tmp_dump = GetTmpDumpDir();
    if (is_dir(tmp_dump.c_str()) == 0 && delete_dir(tmp_dump.c_str()) != 0) {
        LOG(ERROR) << "remove exist tmp dump dir failed";
        return;
    }

    // Initial BackupEngine
    nemo::Status nemo_s = nemo::BackupEngine::Open(
            nemo::BackupableOptions(tmp_dump, true, false), 
            &backup_engine_);
    if (!nemo_s.ok()) {
        LOG(ERROR) << "open backup engine failed " << nemo_s.ToString();
        return;
    }

    {
        RWLock l(&rwlock_, true);
        g_pikaMario->GetProducerStatus(&dump_filenum_, &dump_pro_offset_);
        nemo_s = backup_engine_->SetBackupContent(db_);
        if (!nemo_s.ok()){
            LOG(ERROR) << "set backup content failed " << nemo_s.ToString();
            ClearBackupEngine();
            return;
        }
        bgsaving_ = true;
    }
    
    // Create new thread to do dump
    dump_args *arg = new dump_args;
    arg->p_pika = (void*)this;
    arg->dump_path = dump_path;
    pthread_create(&dump_thread_id_, NULL, &(PikaServer::StartDump), (void*)arg);
}

void* PikaServer::StartDump(void* arg) {
    dump_args *arg_ptr = static_cast<dump_args*>(arg);
    PikaServer* p = static_cast<PikaServer*>(arg_ptr->p_pika);
    std::string dump_path =arg_ptr->dump_path;

    // Backup to tmp dir
    nemo::BackupEngine* backup_engine = p->GetBackupEngine();
    nemo::Status nemo_s = backup_engine->CreateNewBackup(p->GetHandle());
    LOG(INFO) << "create new backup finished :";
    // Restore to dump dir
    if (nemo_s.ok()) {
        nemo_s = backup_engine->RestoreDBFromBackup(
                backup_engine->GetLatestBackupID() + 1, dump_path);
        if (!nemo_s.ok()) {
            LOG(ERROR) << "restore from backup failed :" << nemo_s.ToString();
        }
    } else {
        LOG(ERROR) << "create new backup failed :" << nemo_s.ToString();
    }

    std::string tmp_dump = p->GetTmpDumpDir();
    if (is_dir(tmp_dump.c_str()) == 0 && delete_dir(tmp_dump.c_str()) != 0) {
        LOG(ERROR) << "remove tmp dump dir failed";
    }
    LOG(INFO) << "dump finished.";
    
    // Some output
    time_t delta = time(NULL) - p->bgsaving_start_time_;
    char buf[32];
    snprintf(buf, sizeof(buf), "%lu", delta);
    std::ofstream out;
    out.open(dump_path + "/info", std::ios::in | std::ios::trunc);
    if (out.is_open()) {
        out << buf << "s\r\n";
        out << p->GetServerIp() << "\r\n";
        out << p->GetServerPort() << "\r\n";
        out << p->dump_filenum_ << "\r\n";
        out << p->dump_pro_offset_ << "\r\n";
        out.close();
    }
    if (!nemo_s.ok()) {
        std::string fail_path = dump_path + "_FAILED";
        if (is_dir(fail_path.c_str()) == 0 && delete_dir(fail_path.c_str()) != 0) {
            LOG(ERROR) << "remove exist fail dump dir failed :" << fail_path;
        }
        rename(dump_path.c_str(), fail_path.c_str());
    }

    {
        MutexLock l(p->Mutex());
        p->bgsaving_ = false;
        p->dump_thread_id_ = 0;
        p->ClearBackupEngine();
    }
    delete (dump_args*)arg;
    return NULL;
}

bool PikaServer::Dumpoff() {
    {
        MutexLock l(&mutex_);
        if (!bgsaving_) {
            return false;
        }
    }
    if (backup_engine_ == NULL) {
        return false;
    }
    backup_engine_->StopBackup();
    pthread_t dump_thread_id;
    {
        MutexLock l(&mutex_);
        dump_thread_id = dump_thread_id_;
    }
    if (dump_thread_id != 0) {
        pthread_join(dump_thread_id, NULL);
    }
    return true;
}

bool PikaServer::PurgeLogsNolock(uint32_t max, int64_t to) {
    if (purging_) {
        return false;
    }
    if (to < 0 || to > max) {
        return false;
    }

    purge_args *arg = new purge_args;
    arg->p = (void*)this;
    arg->to = to;
    purging_ = true;
    pthread_create(&purge_thread_id_, NULL, &(PikaServer::StartPurgeLogs), arg);
    return true;
}

bool PikaServer::PurgeLogs(uint32_t max, int64_t to) {
    MutexLock l(&mutex_);
    if (purging_) {
        return false;
    }
//    if (max > 9) {
//        max -= 10;
//    } else {
//        return false;
//    }
    if (to < 0 || to > max) {
        return false;
    }

    purge_args *arg = new purge_args;
    arg->p = (void*)this;
    arg->to = to;
    purging_ = true;
    pthread_create(&purge_thread_id_, NULL, &(PikaServer::StartPurgeLogs), arg);
    return true;
}

int log_filter(std::string path, uint32_t to, std::set<std::string> &files)
{
    DIR *dir;
    struct dirent *entry;
    std::string writefile = "write2file";

    dir = opendir(path.c_str());
    if (!dir) {
        return 0;
    }
    int num = 0;
    while ((entry = readdir(dir))) {
        if (!strncmp(entry->d_name, writefile.c_str(), writefile.length()) && stoul(std::string(entry->d_name).substr(writefile.length())) <= to) {
            num++;
            files.insert(std::string(entry->d_name));
        }
    }
    closedir(dir);

    return num;
}

void* PikaServer::StartPurgeLogs(void* arg) {
    PikaServer* p = (PikaServer*)(((purge_args*)arg)->p);
    uint32_t to = ((purge_args*)arg)->to;

    std::string log_path(g_pikaConf->log_path());
    if (log_path[log_path.length() - 1] != '/') {
        log_path.append("/");
    }
    /*
    char buf[128];
    std::string prefix = log_path + "write2file";
    */
    std::string filename;
    int ret = 0;
    std::set<std::string> files_to_del;
    log_filter(log_path, to, files_to_del);
    for (std::set<std::string>::iterator iter = files_to_del.begin(); iter != files_to_del.end(); iter++) {
        filename = log_path + *iter;
        ret = access(filename.c_str(), F_OK);
        if (ret) continue;
        LOG(INFO) << "Delete " << filename << "...";
        remove(filename.c_str());
    }
    {
    MutexLock l(p->Mutex());
    p->purging_ = false;
    }
    delete (purge_args*)arg;
    return NULL;
}

void PikaServer::InfoKeySpace() {
    MutexLock l(&mutex_);
    if (info_keyspacing_) {
        return;
    }
    
    info_keyspacing_ = true;
    pthread_create(&info_keyspace_thread_id_, NULL, &(PikaServer::StartInfoKeySpace), this);
}

void* PikaServer::StartInfoKeySpace(void* arg) {
    PikaServer* p = (PikaServer*)arg;
    p->info_keyspace_start_time_ = time(NULL);
    p->keynums_.clear();
    p->GetHandle()->GetKeyNum(p->keynums_);

    {
    MutexLock l(p->Mutex());
    p->last_info_keyspace_start_time_ = p->info_keyspace_start_time_;
    p->last_kv_num_ = p->keynums_[0];
    p->last_hash_num_ = p->keynums_[1];
    p->last_list_num_ = p->keynums_[2];
    p->last_zset_num_ = p->keynums_[3];
    p->last_set_num_ = p->keynums_[4];
    p->info_keyspacing_ = false;
    }
    return NULL;
}

void PikaServer::Slaveofnoone() {
    MutexLock l(&mutex_);

    char buf[32];
    snprintf(buf, sizeof(buf), "%d", masterport_);
    std::string masterport(buf);
    std::string masteripport = masterhost_ + ":" + masterport;
    ClientKill(masteripport);
    repl_state_ &= (~ PIKA_SLAVE);
    pthread_rwlock_unlock(&rwlock_);
    {
    RWLock rwl(&rwlock_, true);
    is_readonly_ = false;
    LOG(WARNING) << "Slave of no one , close readonly mode, repl_state_: " << repl_state_;
    }
    g_pikaConf->SetReadonly(false);
    pthread_rwlock_rdlock(&rwlock_);

    ms_state_ = PIKA_REP_SINGLE;
    masterhost_ = "";
    masterport_ = 0;
}

std::string PikaServer::is_bgsaving() {
    MutexLock l(&mutex_);
    std::string s;
    if (bgsaving_) {
        s = "Yes, ";
        s.append(dump_time_);
        time_t delta = time(NULL) - bgsaving_start_time_;
        char buf[32];
        snprintf(buf, sizeof(buf), "%lu", delta);
        s.append(", ");
        s.append(buf);
    } else {
        s = "No, ";
        s.append(dump_time_);
        s.append(", 0");

    }
    return s;
}

std::string PikaServer::is_scaning() {
    MutexLock l(&mutex_);
    std::string s;
    if (info_keyspacing_) {
        s = "Yes, ";
        char infotime[32];
        strftime(infotime, sizeof(infotime), "%Y-%m-%d %H:%M:%S", localtime(&(info_keyspace_start_time_))); 
        s.append(infotime);
        time_t delta = time(NULL) - info_keyspace_start_time_;
        char buf[32];
        snprintf(buf, sizeof(buf), "%lu", delta);
        s.append(", ");
        s.append(buf);
    } else {
        s = "No";
    }
    return s;
}

int log_num(std::string path)
{
    DIR *dir;
    struct dirent *entry;
    std::string writefile = "write2file";

    dir = opendir(path.c_str());
    if (!dir) {
        return 0;
    }
    int num = 0;
    while ((entry = readdir(dir))) {
        if (!strncmp(entry->d_name, writefile.c_str(), writefile.length())) {
            num++;
        }
    }
    closedir(dir);

    return num;
}

int log_max_deadline_index(std::string path, time_t dead_time_s) {
    DIR *dir;
    struct dirent *entry;
    std::string writefile = "write2file";

    dir = opendir(path.c_str());
    if (!dir) {
        return -1;
    }

    if (path[path.size()-1] != '/') {
        path.push_back('/');
    }

    int max_index = -1, index, ret;
    struct stat file_stat;
    while ((entry = readdir(dir))) {
        if (strncmp(entry->d_name, writefile.c_str(), writefile.length())) {
            continue;
        }
        ret = stat((path+entry->d_name).c_str(), &file_stat);
        if (ret != 0) continue;
        index = stol(std::string(entry->d_name).substr(writefile.length()));
        if (max_index < index && file_stat.st_mtime <= dead_time_s) {
            max_index = index;
        }
    }
    closedir(dir);
    return max_index;
}

void PikaServer::AutoPurge() {
//    time_t current_time_s = std::time(NULL);
//    struct tm *current_time_tm_ptr = localtime(&current_time_s);
//    int32_t diff_year = current_time_tm_ptr->tm_year - last_autopurge_time_tm_.tm_year;
//    int32_t diff_day = current_time_tm_ptr->tm_yday - last_autopurge_time_tm_.tm_yday;
//    int32_t running_time_d = diff_year*365 + diff_day;
    int32_t num = log_num(g_pikaConf->log_path());
    int32_t expire_logs_nums = g_pikaConf->expire_logs_nums();
    int32_t expire_logs_days = g_pikaConf->expire_logs_days();
//    if (running_time_d > g_pikaConf->expire_logs_days() - 1) {
//        uint32_t max = 0;
//        g_pikaMario->GetStatus(&max);
//        if(max >= 10 && PurgeLogs(max, max-10)) {
//            LOG(WARNING) << "Auto Purge(Days): write2file" << max-10 << " interval days: " << running_time_d;
//            save_time_tm(&last_autopurge_time_tm_, current_time_tm_ptr);
//        }
//    } else if (num > expire_logs_nums) {
    uint32_t max = 0;
    g_pikaMario->GetStatus(&max);
    if (num > expire_logs_nums) {
        if (max >= 10 && PurgeLogs(max, max-expire_logs_nums)) {
            LOG(WARNING) << "Auto Purge(Nums): write2file" << max-expire_logs_nums << " log nums: " << num;
        }
    }
    time_t current_time_s = time(NULL), last_purge_time_s;
    {
        MutexLock l(&mutex_);
        last_purge_time_s = last_purge_time_s_;
    }
    if (current_time_s - last_purge_time_s >= expire_logs_days*24*3600) {
        std::string path = g_pikaConf->log_path();
        int max_deadline_index = log_max_deadline_index(path, last_purge_time_s+24*3600);
        if (max_deadline_index == -1 || (max>=10 && PurgeLogs(max-10, max_deadline_index))) {
            LOG(WARNING) << "Auto Purge(Days): write2file, No." << (last_purge_time_s-start_time_s_)/(24*3600) + 1 << " day " << num;
            MutexLock l(&mutex_);
            last_purge_time_s_ = last_purge_time_s_ + 24*3600;
        }
    }
}

void PikaServer::ProcessTimeEvent(struct timeval* target) {
    std::string ip_port;
    char buf[32];
    target->tv_sec++;
    AutoPurge();

    {
    MutexLock l(&mutex_);
    if (ms_state_ == PIKA_REP_CONNECT) {
        //connect
        LOG(WARNING) << "try to connect with master: " << masterhost_ << ":" << masterport_;
        struct sockaddr_in s_addr;
        int connfd = socket(AF_INET, SOCK_STREAM, 0);
        if (connfd == -1) {
            LOG(WARNING) << "socket error " << strerror(errno);
            return ;
        }
        memset(&s_addr, 0, sizeof(s_addr));
        if (connfd == -1) {
            return ;
        }
        int flags = fcntl(connfd, F_GETFL, 0);
        fcntl(connfd, F_SETFL, flags | O_NONBLOCK);
        int yes = 1;
        if (setsockopt(connfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
            LOG(WARNING) << "setsockopt SO_REUSEADDR: " << strerror(errno);
            return ;
        }
        if (setsockopt(connfd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes)) == -1) {
            LOG(WARNING) << "setsockopt SO_KEEPALIVE: " << strerror(errno);
            return ;
        }
        s_addr.sin_family = AF_INET;
        s_addr.sin_addr.s_addr = inet_addr(masterhost_.c_str());
        s_addr.sin_port = htons(masterport_);
        if (-1 == connect(connfd, (struct sockaddr*)(&s_addr), sizeof(s_addr))) {
            if (errno == EINPROGRESS) {
                struct pollfd   wfd[1];
                wfd[0].fd     = connfd;
                wfd[0].events = POLLOUT;

                int res;
                if ((res = poll(wfd, 1, 600)) == -1) {
                    close(connfd);
                    LOG(WARNING) << "The target host cannot be reached";
                    return ;
                } else if (res == 0) {
                    errno = ETIMEDOUT;
                    close(connfd);
                    LOG(WARNING) << "The target host connect timeout";
                    return ;
                }
                int err = 0;
                socklen_t errlen = sizeof(err);

                if (getsockopt(connfd, SOL_SOCKET, SO_ERROR, &err, &errlen) == -1) {
                    close(connfd);
                    LOG(WARNING) << "The target host cannot be reached";
                    return ; 
                }

                if (err) {
                    errno = err;
                    close(connfd);
                    LOG(WARNING) << "The target host cannot be reached";
                    return ;
                }
            }
        }
         
        char ipAddr[INET_ADDRSTRLEN] = "";
        ip_port = inet_ntop(AF_INET, &s_addr.sin_addr, ipAddr, sizeof(ipAddr));
        ip_port.append(":");
        ll2string(buf, sizeof(buf), ntohs(s_addr.sin_port));
        ip_port.append(buf);
        std::queue<PikaItem> *q = &(pikaThread_[thread_num_-1]->conn_queue_);
        LOG(WARNING) << "Push Master to Thread " << thread_num_-1;
        PikaItem ti(connfd, ip_port, PIKA_MASTER);
        {
            MutexLock l(&pikaThread_[thread_num_-1]->mutex_);
            q->push(ti);
        }
        write(pikaThread_[thread_num_-1]->notify_send_fd(), "", 1);
        repl_state_ |= PIKA_SLAVE;
        ms_state_ = PIKA_REP_CONNECTING;
    }
    }
}

void PikaServer::DisconnectFromMaster() {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", masterport_);
    std::string str_port(buf);
    std::string master_ip_port = masterhost_ + ":" + str_port;
    ClientKill(master_ip_port);
    {
    MutexLock l(&mutex_);
    masterhost_ = "";
    masterport_ = 0;
    repl_state_ &= (~ PIKA_SLAVE);
    LOG(WARNING) << "Disconnect with master, " << repl_state_;
    ms_state_ = PIKA_REP_SINGLE;
    }
}

int PikaServer::TrySync(/*std::string &ip, std::string &str_port,*/ int fd, uint64_t filenum, uint64_t offset) {
//    std::string ip_port = ip + ":" + str_port;
//    std::map<std::string, SlaveItem>::iterator iter = slaves_.find(ip_port);
//    int64_t port;
//    string2l(str_port.data(), str_port.size(), &port);
//    if (iter != slaves_.end()) {
//        return PIKA_REP_STRATEGY_ALREADY;
//    }
    std::map<int, PikaConn*>::iterator iter_fd;
    PikaConn* conn = NULL;
    int i = 0;
    for (i = g_pikaConf->thread_num(); i < thread_num_; i++) {
        iter_fd = pikaThread_[i]->conns()->find(fd);
        if (iter_fd != pikaThread_[i]->conns()->end()) {
            conn = iter_fd->second;
            break;
        }
    }
    if (conn == NULL) {
        return PIKA_REP_STRATEGY_ERROR;
    }

    std::map<std::string, client_info>::iterator iter_cl;
    MarioHandler* h = new MarioHandler(conn);
    mario::Status s = g_pikaMario->AddConsumer(filenum, offset, h, fd);
    if (s.ok()) {
        {
        MutexLock l(&mutex_);
        repl_state_ |= PIKA_MASTER;
        }
//        {
//        MutexLock l(&mutex_);
//        SlaveItem ss;
//        ss.ip = ip;
//        ss.port = port;
//        ss.state = PIKA_REP_CONNECTED;
//        slaves_[ip_port] = ss;
//        std::map<std::string, SlaveItem>::iterator ii = slaves_.begin();
//        while (ii != slaves_.end()) {
//            LOG(INFO) << ii->first << " " << (ii->second).ip << " " <<  (ii->second).port <<" "<< (ii->second).state;
//            ii++;
//        }
//        }
        {
        RWLock l(pikaThread_[i]->rwlock(), true);
        iter_cl = pikaThread_[i]->clients()->find(iter_fd->second->ip_port());
        if (iter_cl != pikaThread_[i]->clients()->end()) {
            LOG(WARNING) << "Set client role to slave";
            iter_cl->second.role = PIKA_SLAVE;
        }
        }
        conn->set_role_nolock(PIKA_SLAVE);
        return PIKA_REP_STRATEGY_PSYNC;
    } else {
        return PIKA_REP_STRATEGY_ERROR;
    }
}

//void PikaServer::Offline(std::string ip_port) {
//    std::map<std::string, SlaveItem>::iterator iter = slaves_.find(ip_port);
//    if (iter != slaves_.end()) {
//        iter->second.state = PIKA_REP_OFFLINE;
//    }
//}

int PikaServer::ClientList(std::string &res) {
    int client_num = 0;
    std::map<std::string, client_info>::iterator iter;
    res = "+";
    char buf[32];
    for (int i = 0; i < thread_num_; i++) {

        {
            RWLock l(pikaThread_[i]->rwlock(), false);
            iter = pikaThread_[i]->clients()->begin();
            while (iter != pikaThread_[i]->clients()->end()) {
                res.append("addr=");
                res.append(iter->first);
                res.append(" fd=");
                ll2string(buf,sizeof(buf), (iter->second).fd);
                res.append(buf);
                res.append("\n");
                client_num++;
                iter++;
            }
        }
    }
    res.append("\r\n");

    return client_num;
}

int PikaServer::GetSlaveList(std::string &res) {
  std::map<std::string, client_info>::iterator iter;
  res = "";
  int slave_num = 0;
  char buf[512];

  for (int i = g_pikaConf->thread_num(); i < thread_num_; i++) {
    {
      RWLock l(pikaThread_[i]->rwlock(), false);
      for (iter = pikaThread_[i]->clients()->begin();
           iter != pikaThread_[i]->clients()->end(); iter++) {
        if (iter->second.role == PIKA_SLAVE) {
          snprintf (buf, sizeof(buf),
                    "slave%d: host_port=%s state=online\r\n",
                    slave_num, iter->first.c_str());
          res.append(buf);
          slave_num++;
        }
      }
    }
  }

  return slave_num;
}

int PikaServer::ClientNum() {
    int client_num = 0;
    std::map<std::string, client_info>::iterator iter;
    for (int i = 0; i < thread_num_; i++) {
        {
            RWLock l(pikaThread_[i]->rwlock(), false);
            iter = pikaThread_[i]->clients()->begin();
            while (iter != pikaThread_[i]->clients()->end()) {
                client_num++;
                iter++;
            }
        }
    }
    return client_num;
}

int PikaServer::ClientKill(std::string &ip_port) {
    int i = 0;
    std::map<std::string, client_info>::iterator iter;
    for (i = 0; i < thread_num_; i++) {
        {
            RWLock l(pikaThread_[i]->rwlock(), true);
            iter = pikaThread_[i]->clients()->find(ip_port);
            if (iter != pikaThread_[i]->clients()->end()) {
                (iter->second).is_killed = true;
                break;
            }
        }
    }
    if (i < thread_num_) {
        return 1;
    } else {
        return 0;
    }

}

void PikaServer::ClientKillAll() {
    int i = 0;
    std::map<std::string, client_info>::iterator iter;
    for (i = 0; i < g_pikaConf->thread_num(); i++) {
        {
            RWLock l(pikaThread_[i]->rwlock(), true);
            iter = pikaThread_[i]->clients()->begin();
            while (iter != pikaThread_[i]->clients()->end()) {
                if ((iter->second).role == PIKA_SINGLE) {
                    (iter->second).is_killed = true;
                }
                iter++;
            }
        }
    }
}

int PikaServer::CurrentQps() {
    int i = 0;
    int qps = 0;
    std::map<std::string, client_info>::iterator iter;
    for (i = 0; i < thread_num_; i++) {
        {
            RWLock l(pikaThread_[i]->rwlock(), false);
            qps+=pikaThread_[i]->last_sec_querynums_;
        }
    }
    return qps;
}

uint64_t PikaServer::CurrentAccumulativeQueryNums()
{
	int i = 0;
	uint64_t accumulativeQueryNums = 0;
	std::map<std::string, client_info>::iterator iter;
	for(i = 0; i < thread_num_; i++)
	{
		RWLock l(pikaThread_[i]->rwlock(), false);
		accumulativeQueryNums += pikaThread_[i]->accumulative_querynums_;
	}
	return accumulativeQueryNums;
}

uint64_t PikaServer::HistoryClientsNum()
{
  MutexLock l(&mutex_);
  return history_clients_num_;
}

//int PikaServer::ClientRole(int fd, int role) {
//    int i = 0;
//    std::map<int, PikaConn*>::iterator iter_fd;
//    std::map<std::string, client_info>::iterator iter;
//    for (i = 0; i < g_pikaConf->thread_num(); i++) {
//
//        if (role == CLIENT_MASTER) {
//            RWLock l(pikaThread_[i]->rwlock(), false);
//            iter = pikaThread_[i]->clients()->begin();
//            while (iter != pikaThread_[i]->clients()->end()) {
//                if (iter->second.role == CLIENT_MASTER && iter->second.fd != fd) {
//                    iter->second.role = CLIENT_NORMAL;
//                    iter->second.is_killed = true;
//                    break;
//                }
//                iter++;
//            }
//        }
//
//        {
//            iter_fd = pikaThread_[i]->conns()->find(fd);
//            if (iter_fd != pikaThread_[i]->conns()->end()) {
//                RWLock l(pikaThread_[i]->rwlock(), true);
//                iter = pikaThread_[i]->clients()->find(iter_fd->second->ip_port());
//                if (iter != pikaThread_[i]->clients()->end()) {
//                    (iter->second).role = role;
//                    break;
//                }
//            }
//        }
//    }
//    if (i < g_pikaConf->thread_num()) {
//        return 1;
//    } else {
//        return 0;
//    }
//
//}

void* PikaServer::StartThread(void* arg)
{
    reinterpret_cast<PikaThread*>(arg)->RunProcess();
    return NULL;
}


void PikaServer::RunProcess()
{
    int nfds;
    PikaFiredEvent *tfe;
    Status s;
    struct sockaddr_in cliaddr;
    socklen_t clilen = sizeof(cliaddr);
    int fd, connfd;
    char ipAddr[INET_ADDRSTRLEN] = "";
    std::string ip_port;
	  std::string ip_str;
    char buf[32];

    struct timeval target;
    struct timeval now;
    gettimeofday(&target, NULL);
    target.tv_sec++;
    int timeout = 1000;
    for (;;) {
        if (shutdown && worker_num == 0) {
            return;
        }

        gettimeofday(&now, NULL);
        if (target.tv_sec > now.tv_sec || (target.tv_sec == now.tv_sec && target.tv_usec - now.tv_usec > 1000)) {
            timeout = (target.tv_sec-now.tv_sec)*1000 + (target.tv_usec-now.tv_usec)/1000;
        } else {
            ProcessTimeEvent(&target);
            timeout = 1000;
        }
        nfds = pikaEpoll_->PikaPoll(timeout);
        tfe = pikaEpoll_->firedevent();
        for (int i = 0; i < nfds; i++) {
            fd = (tfe + i)->fd_;
            if (fd == sockfd_ && ((tfe + i)->mask_ & EPOLLIN)) {
                connfd = accept(sockfd_, (struct sockaddr *) &cliaddr, &clilen);
                if (connfd == -1) {
                    if (errno != EWOULDBLOCK) {
                        LOG(WARNING) << "too open files, can not accpet client";
                        continue;
                    }
                }
//                LOG(INFO) << "Accept new connection, fd: " << connfd << " ip: " << inet_ntop(AF_INET, &cliaddr.sin_addr, ipAddr, sizeof(ipAddr)) << " port: " << ntohs(cliaddr.sin_port);
                //ip_port = inet_ntop(AF_INET, &cliaddr.sin_addr, ipAddr, sizeof(ipAddr));
                ip_str = inet_ntop(AF_INET, &cliaddr.sin_addr, ipAddr, sizeof(ipAddr));
                ip_port = ip_str;
                ip_port.append(":");
                ll2string(buf, sizeof(buf), ntohs(cliaddr.sin_port));
                ip_port.append(buf);
                int clientnum = ClientNum();
                if ((clientnum >= g_pikaConf->maxconnection() + g_pikaConf->root_connection_num())
				              || ((clientnum >= g_pikaConf->maxconnection()) && (ip_str != std::string("127.0.0.1") && (ip_str != GetServerIp())))) {
                    LOG(WARNING) << "Reach Max Connection: "<< g_pikaConf->maxconnection() << " refuse new client: " << ip_port;
                    close(connfd);
                    continue;
                }
                std::queue<PikaItem> *q = &(pikaThread_[last_thread_]->conn_queue_);
                PikaItem ti(connfd, ip_port);
                LOG(INFO) << "Push Client to Thread " << (last_thread_);
                {
                    MutexLock l(&pikaThread_[last_thread_]->mutex_);
                    q->push(ti);
                }
                write(pikaThread_[last_thread_]->notify_send_fd(), "", 1);
                last_thread_++;
                last_thread_ %= g_pikaConf->thread_num();
                {
                  MutexLock l(&mutex_);
                  history_clients_num_++;
                }
            } else if (fd == slave_sockfd_ && ((tfe + i)->mask_ & EPOLLIN)) {
                connfd = accept(slave_sockfd_, (struct sockaddr *) &cliaddr, &clilen);
                if (connfd == -1) {
                    if (errno != EWOULDBLOCK) {
                        LOG(WARNING) << "too open files, can not accpet slave";
                        continue;
                    }
                }
//                LOG(INFO) << "Accept new connection, fd: " << connfd << " ip: " << inet_ntop(AF_INET, &cliaddr.sin_addr, ipAddr, sizeof(ipAddr)) << " port: " << ntohs(cliaddr.sin_port);
                ip_port = inet_ntop(AF_INET, &cliaddr.sin_addr, ipAddr, sizeof(ipAddr));
                ip_port.append(":");
                ll2string(buf, sizeof(buf), ntohs(cliaddr.sin_port));
                ip_port.append(buf);

                int user_thread_num = g_pikaConf->thread_num();
                std::queue<PikaItem> *q = &(pikaThread_[last_slave_thread_ + user_thread_num]->conn_queue_);
                PikaItem ti(connfd, ip_port);
                LOG(WARNING) << "Push Slave " << ip_port << " to Thread " << (last_slave_thread_ + user_thread_num);
                {
                    MutexLock l(&pikaThread_[last_slave_thread_ + user_thread_num]->mutex_);
                    q->push(ti);
                }
                write(pikaThread_[last_slave_thread_ + user_thread_num]->notify_send_fd(), "", 1);
                last_slave_thread_++;
                last_slave_thread_ %= g_pikaConf->slave_thread_num();
#ifndef EPOLLRDHUP
            } else if ((tfe + i)->mask_ & (EPOLLERR | EPOLLHUP)) {
#else
            } else if ((tfe + i)->mask_ & (EPOLLRDHUP | EPOLLERR | EPOLLHUP)) {
#endif
                LOG(WARNING) << "Epoll timeout event";
            }
        }
    }
}