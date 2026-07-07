
#include "detail/logger.h"
#include "detail/flogview.h"
#include <backward.hpp>
#include <ctime>
#include <atomic>
#include <csignal>
// #include <syncstream>

namespace { // anonymous namespace for internal linkage


constexpr std::uintmax_t MAX_TASK_LOG_FOLDER_SIZE = 1024 * 1024 * 1024; // 1 GB
constexpr uint_fast32_t MAX_FILE_LOG_LINE_COUNT = 300 * 1000; // 300K lines
// constexpr std::uintmax_t MAX_TASK_LOG_FOLDER_SIZE = 1024 * 1024;
// constexpr uint_fast32_t MAX_FILE_LOG_LINE_COUNT = 20;

void tuiThreadFunc(void) {
    finalcut::FApplication::getApplicationObject()->exec();
}

constexpr std::string_view _logLevelToString(uOS::LogLevel level) {
    switch (level) {
        case uOS::LogLevel::LOG_TRACE: return "TRACE   ";
        case uOS::LogLevel::LOG_INFO: return "INFO    ";
        case uOS::LogLevel::LOG_WARNING: return "WARNING ";
        case uOS::LogLevel::LOG_ERROR: return "ERROR   ";
        default: return "UNKNOWN";
    }
}

constexpr std::string_view _logLevelToColor(uOS::LogLevel level) {
    switch (level) {
        case uOS::LogLevel::LOG_WARNING: return "\033[38;5;208m"; // Orange
        case uOS::LogLevel::LOG_ERROR: return "\033[31m"; // Red
        default: return "UNKNOWN-COLOR";
    }
}

// Fast, one‑pass UTF‑8 → wchar_t (assumes wchar_t is at least 32 bits)
// AI dan copy/paste. kontrol edilecek
inline std::wstring _utf8ToWstring(const std::string& utf8)
{
    const unsigned char* ptr = reinterpret_cast<const unsigned char*>(utf8.data());
    const unsigned char* end = ptr + utf8.size();

    std::wstring out;
    out.reserve(utf8.size());  // worst‑case: 1 byte → 1 wchar_t

    while (ptr < end) {
        uint32_t codepoint = 0;
        size_t    seqlen    = 0;
        unsigned char lead = *ptr;

        if (lead < 0x80) {
            codepoint = lead;
            seqlen = 1;
        }
        else if ((lead >> 5) == 0x6) {
            // if (ptr + 1 >= end) throw std::runtime_error("Invalid UTF-8");
            if (ptr + 1 >= end) assert(false);
            codepoint  = (lead & 0x1F) << 6;
            codepoint |= (ptr[1] & 0x3F);
            seqlen = 2;
        }
        else if ((lead >> 4) == 0xE) {
            // if (ptr + 2 >= end) throw std::runtime_error("Invalid UTF-8");
            if (ptr + 2 >= end) assert(false);
            codepoint  = (lead & 0x0F) << 12;
            codepoint |= (ptr[1] & 0x3F) << 6;
            codepoint |= (ptr[2] & 0x3F);
            seqlen = 3;
        }
        else if ((lead >> 3) == 0x1E) {
            // if (ptr + 3 >= end) throw std::runtime_error("Invalid UTF-8");
            if (ptr + 3 >= end) assert(false);
            codepoint  = (lead & 0x07) << 18;
            codepoint |= (ptr[1] & 0x3F) << 12;
            codepoint |= (ptr[2] & 0x3F) << 6;
            codepoint |= (ptr[3] & 0x3F);
            seqlen = 4;
        }
        else {
            // throw std::runtime_error("Invalid UTF-8 lead byte");
            assert(false);
        }

        out.push_back(static_cast<wchar_t>(codepoint));
        ptr += seqlen;
    }

    return out;
}

void _folderSizer(std::filesystem::path folderPath){
    static std::mutex folderSizerMutex;

    // make function thread safe to avoid race condition for deleting same file
    std::lock_guard<std::mutex> lock(folderSizerMutex);
    
    // calculate folder size
    std::uintmax_t totalSize = 0;
    std::vector<std::filesystem::path> logFiles;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(folderPath, ec)) {
        // check for error
        if (ec) continue;

        std::error_code ec2;
        if (entry.is_regular_file(ec2) && !ec2 && entry.path().extension() == ".log") {
            totalSize += std::filesystem::file_size(entry);
            logFiles.push_back(entry.path());
        }
    }
    
    // delete oldest log file if folder size is greater than limit
    if(totalSize > MAX_TASK_LOG_FOLDER_SIZE) {
        // Lexicographical compare on filenames
        auto comp = [](const std::filesystem::path& a, const std::filesystem::path& b){
            return a.filename().string() < b.filename().string();
        };
        auto oldestFile = std::min_element(logFiles.begin(), logFiles.end(), comp);
        
        // remove the oldest file
        LOG(uOS::LogLevel::LOG_INFO, "Log size in folder: {} is {} bytes. Deleting oldest log file: {}", folderPath.filename().string(), totalSize, oldestFile->filename().string());
        if(std::filesystem::remove(*oldestFile) == false) {
            ASSERT_FOR_FATAL(false);
        }
    } else {
        LOG(uOS::LogLevel::LOG_TRACE, "Log size in folder: {} is {} bytes", folderPath.filename().string(), totalSize);
    }
}


} // namespace anonymous




namespace uOS {


void Logger::init(int argc, char* argv[], std::optional<std::function<void(void)>> quitCb)
{
    bool logsCreated = false;
    bool systemCreated = false;
    const std::string_view logsRootFolderName = "LOGS";
    const std::string_view systemLogsFolderName = "_SYSTEM";
    std::filesystem::path systemFolderPath;

    // start TUI if argc not null
    if(quitCb.has_value()) {
        //static finalcut::FApplication app(argc, argv);
        _tui = new finalcut::FApplication(argc, argv);

        // Force terminal initialization without calling show()
        _tui->initTerminal();

        // create logger
        //_loggerFw = new FLogView(_tui, 2000);
        _loggerTasks = new FLogViewMulti(_tui, 1000);
        finalcut::FWidget::setMainWidget(_loggerTasks);

        // configure loger
        _loggerTasks->setText(L"Logger");
        _loggerTasks->unsetShadow();
        _loggerTasks->setResizable(true);
        _loggerTasks->setGeometry(finalcut::FPoint{1,1}, finalcut::FSize{_tui->getDesktopWidth(), _tui->getDesktopHeight()});
        _loggerTasks->registerOnQuit(quitCb.value());
        _loggerTasks->show();  // todo: bunu start'a taşıyabiliriz
    }

    // initialize file logger
    // get binary path in buffer
    char buffer[1024];
    ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    ASSERT_FOR_FATAL(len != -1);
    buffer[len] = '\0';

    // create LOGS folder if necessary
    _logFolderPath = std::filesystem::path(buffer).parent_path() / logsRootFolderName;
    if (std::filesystem::exists(_logFolderPath) == false) {
        if (std::filesystem::create_directory(_logFolderPath)) {
            logsCreated = true;
        } else {
            ASSERT_FOR_FATAL(false);
        }
    }
    
    // create _SYSTEM folder if necessary
    systemFolderPath = _logFolderPath / systemLogsFolderName;
    if(std::filesystem::exists(systemFolderPath) == false){
        if (std::filesystem::create_directory(systemFolderPath)) {
            systemCreated = true;
        } else {
            ASSERT_FOR_FATAL(false);
        }
    }

    // create system log file
    std::time_t now = std::time(nullptr);
    std::tm* timeStruct = std::localtime(&now); 
    std::ostringstream fileNameStream;
    fileNameStream  << std::setw(2) << std::setfill('0') << ((timeStruct->tm_year + 1900) % 100)
                    << std::setw(2) << std::setfill('0') << (timeStruct->tm_mon + 1)
                    << std::setw(2) << std::setfill('0') << timeStruct->tm_mday
                    << std::setw(2) << std::setfill('0') << timeStruct->tm_hour
                    << std::setw(2) << std::setfill('0') << timeStruct->tm_min
                    << std::setw(2) << std::setfill('0') << timeStruct->tm_sec;
    
    std::ofstream systemFile(systemFolderPath / (fileNameStream.str() + ".log"));
    ASSERT_FOR_FATAL(systemFile.is_open());

    // add to system file
    _systemFile = std::make_unique<TaskLogStream>(std::move(systemFile), std::move(systemFolderPath), fileNameStream.str());

    // enable crash backtrace reporting
    std::signal(SIGSEGV, _systemSignalHandler); // Segmentation fault
    std::signal(SIGABRT, _systemSignalHandler); // Abort signal
    std::signal(SIGILL, _systemSignalHandler);  // Illegal instruction
    std::signal(SIGBUS, _systemSignalHandler);  // Bus error

    // print info
    if(quitCb.has_value()){
        LOG(LogLevel::LOG_INFO, "Logger initialized with TUI");
    } else {
        LOG(LogLevel::LOG_INFO, "Logger initialized without TUI");
    }
    if(logsCreated){
        LOG(LogLevel::LOG_INFO, "Root log folder created");
    }
    if(systemCreated){
        LOG(LogLevel::LOG_INFO, "System logs folder created");
    }
}

void Logger::start(void) {
    if(_tui != nullptr){
        _loggerThread = std::thread{tuiThreadFunc};
    }
}

void Logger::stop(void) {
    // close files
    {
        std::lock_guard sysFileLock(_systemFile->fileMutex);
        _systemFile->fileStream.close();
    }

    std::shared_lock taskFileMapLock(_taskFileMapMutex);
    for (auto& [taskId, taskLogStream] : _taskFileMap) {
        std::lock_guard<std::mutex> taskFileLock(taskLogStream.fileMutex);
        taskLogStream.fileStream.close();
    }
    taskFileMapLock.unlock();
    
    // shut down TUI and its thread
    if (_tui) {
        _tui->quit();
        _loggerThread.join();
        delete _tui;
        _tui = nullptr;
    }

    if (_loggerThread.joinable()) {
        _loggerThread.join();
    }
}

void Logger::addTaskLogger(const TaskId taskId, const std::string& taskName, const std::string& instName) {
    // create task folder if necessary
    std::filesystem::path taskFolderPath = _logFolderPath / taskName;
    if (std::filesystem::exists(taskFolderPath)) {
        // check folder size if exists
        _folderSizer(taskFolderPath);
    } else {
        if (std::filesystem::create_directory(taskFolderPath)) {
            LOG(LogLevel::LOG_INFO, "Task log folder created: {}", taskFolderPath.filename().string());
        } else {
            ASSERT_FOR_FATAL(false);
        }
    }

    // form file name
    // name part of the file name
    std::string fileNameIdPart = instName + '_' + taskName + '_' + std::to_string(taskId);

    // timestamp part of the file name
    // get current time
    auto now = std::chrono::system_clock::now();
    // calendar conversion
    auto tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_r(&tt, &tm);
    // get subsecond part
    auto since_epoch = now.time_since_epoch();
    auto secs   = std::chrono::duration_cast<std::chrono::seconds>(since_epoch);
    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(since_epoch - secs).count();
    // format into YYMMDDHHMMSSµµµµµµ
    std::ostringstream fileTimeStampPart;
    fileTimeStampPart << std::setw(2) << std::setfill('0') << ((tm.tm_year + 1900) % 100)
                  << std::setw(2) << std::setfill('0') << (tm.tm_mon + 1)
                  << std::setw(2) << std::setfill('0') << tm.tm_mday
                  << std::setw(2) << std::setfill('0') << tm.tm_hour
                  << std::setw(2) << std::setfill('0') << tm.tm_min
                  << std::setw(2) << std::setfill('0') << tm.tm_sec
                  << std::setw(6) << std::setfill('0') << micros << '_';

    // form the full file name
    std::string fileName = fileTimeStampPart.str() + fileNameIdPart;

    // Open file for writing
    std::ofstream taskfile(taskFolderPath / (fileName + ".log"));
    
    // Check if file was opened successfully
    if (!taskfile.is_open()) {
        ASSERT_FOR_FATAL(false);
    }

    // add to task file map
    {
        std::lock_guard lock(_taskFileMapMutex);
        auto [iter, result] = _taskFileMap.try_emplace(taskId, std::move(taskfile), std::move(taskFolderPath), std::move(fileName));
        ASSERT_FOR_FATAL(result);
    }
    LOG(LogLevel::LOG_INFO, "Log file created for instance: {} of type: {}", instName, taskName);
    
    // add tui view if tui is active
    if(_tui != nullptr){
        _loggerTasks->createView(static_cast<uint_fast16_t>(taskId), taskName);
    }
}

void Logger::removeTaskLogger(const TaskId taskId) {
    // close log file and remove from map
    {
        std::lock_guard lock(_taskFileMapMutex);
        auto iter = _taskFileMap.find(taskId);
        ASSERT_FOR_FATAL(iter != _taskFileMap.end());
        iter->second.fileStream.close();
        _taskFileMap.erase(iter);
    }
    
    // remove from tui view if tui is active
    if(_tui != nullptr){
        _loggerTasks->removeView(static_cast<uint_fast16_t>(taskId));
    }
}

void Logger::logFw(const char* file, const char* function, const int line, const LogLevel level, const std::string& formattedLogStr) {
    _logCommon(file, function, line, level, formattedLogStr);
}

void Logger::logTask(const char* file, const char* function, const int line, const LogLevel level, const std::string& formattedLogStr, TaskId taskId, std::string_view taskIdentifier) {
    _logCommon(file, function, line, level, formattedLogStr, taskId, taskIdentifier);
}

bool Logger::isActive(void) {
    if(_tui == nullptr){
        return false;
    } else {
        return true;
    }
}

//////////////////////////////////////////////////////
//                                                  //
//////////////////////////////////////////////////////

inline void Logger::_logCommon(const char* file, const char* function, const int line, const LogLevel level, const std::string& logStr, std::optional<TaskId> taskId, std::string_view taskIdentifier) {
    // Get current time with high precision
    auto now = std::chrono::system_clock::now();
    std::time_t now_time_t = std::chrono::system_clock::to_time_t(now);

    // Cache localtime_r result per second
    // this is done to avoid the overhead of calling localtime_r for every log line
    static std::atomic<std::time_t> last_sec{0};
    static std::tm cached_tm;
    std::tm timeStruct;
    if (last_sec.load(std::memory_order_relaxed) != now_time_t) {
        localtime_r(&now_time_t, &cached_tm);
        last_sec.store(now_time_t, std::memory_order_relaxed);
    }
    timeStruct = cached_tm;

    // Extract microseconds
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()) % 1000000;

    // Extract base file name (safe)
    const char* baseFileName = std::strrchr(file, '/');
    baseFileName = baseFileName ? baseFileName + 1 : file;

    // Fast formatting with fmt
    std::string logLine = fmt::format("{:02}:{:02}:{:02}.{:06d} {}:{}:{} [|] {}",
        timeStruct.tm_hour, timeStruct.tm_min, timeStruct.tm_sec, static_cast<int>(us.count()),
        baseFileName, line, function, logStr);

    // write to log file
    if(level != LogLevel::LOG_TRACE){
        if(taskId){
            std::shared_lock taskFileMapLock(_taskFileMapMutex);
            auto iter = _taskFileMap.find(taskId.value());
            ASSERT_FOR_FATAL(iter != _taskFileMap.end());
            std::lock_guard<std::mutex> fileLock(iter->second.fileMutex);
            iter->second.fileStream << _logLevelToString(level) << logLine << '\n';
            _fileSizer(iter->second);
        } else {
            std::lock_guard<std::mutex> fileLock(_systemFile->fileMutex);
            _systemFile->fileStream << _logLevelToString(level) << logLine << '\n';
            _fileSizer(*_systemFile);
        }
    }

    // print to tui or cout
    if(_tui != nullptr) {
        std::wstring wstr = _utf8ToWstring(logLine);

        if(taskId){
            _loggerTasks->log(std::move(wstr), static_cast<FLogView::LogLevel>(level), static_cast<uint16_t>(taskId.value()));
        } else {
            // todo: handle system log on TUI
            // _loggerFw->log(std::move(wstr), static_cast<FLogView::LogLevel>(level));
        }
    } else {
        // print to cout if TUI is not active and level is above TRACE
        if(level != LogLevel::LOG_TRACE) {
            constexpr std::string_view colorReset = "\033[0m";

            // Identifier: task name/instance if available, otherwise "SYSTEM"
            std::string_view identifier = taskId ? taskIdentifier : "SYSTEM";
            if (level != LogLevel::LOG_INFO) {
                // Colored output for WARNING/ERROR
                fmt::print("{}{}{} {} [|] {}\n",
                    _logLevelToColor(level),      // e.g. "\033[38;5;208m" or "\033[31m"
                    _logLevelToString(level),     // e.g. "WARNING "
                    colorReset,
                    identifier,
                    logStr);
            } else {
                // No color for INFO
                fmt::print("{} {} [|] {}\n",
                    _logLevelToString(level),
                    identifier,
                    logStr);
            }
        }
    }
}

inline void Logger::_fileSizer(TaskLogStream& logStream) {
    if(++logStream.fileLineCount > MAX_FILE_LOG_LINE_COUNT) {
        // close current file and open new one. check folder size
        logStream.fileStream << "Max line count exceeded, creating new file";
        logStream.fileStream.close();
        logStream.overflowCount++;
        logStream.fileLineCount = 0;
        _folderSizer(logStream.folderPath);
        logStream.fileStream.open(logStream.folderPath / (logStream.fileName + '_' + std::to_string(logStream.overflowCount) + ".log"));
        if (logStream.fileStream.is_open() == false) {
            ASSERT_FOR_FATAL(false);
        }
    }
}

void Logger::_systemSignalHandler(int signum) {
    // Capture backtrace up to 32 frames
    backward::StackTrace st; 
    st.load_here(32);
    
    // Configure printer
    backward::Printer p;
    p.object = false;
    p.address = false;
    p.snippet = true;   // show code snippets
    p.color_mode = backward::ColorMode::automatic;  // color in terminal

    // Handle system signals (e.g., segmentation fault)
    switch (signum) {
        case SIGSEGV: _systemFile->fileStream << "\nSegmentation fault occurred."; break;
        case SIGABRT: _systemFile->fileStream << "\nAbort signal received."; break;
        case SIGILL:  _systemFile->fileStream << "\nIllegal instruction encountered."; break;
        case SIGBUS:  _systemFile->fileStream << "\nBus error occurred."; break;
        default:      _systemFile->fileStream << "\nUnhandled signal (" << signum << ") received."; break;
    }
    _systemFile->fileStream << " Printing backtrace...\n";

    // Add more signal handling as needed

    // Print to system log file
    p.print(st, _systemFile->fileStream);

    // stop logger
    stop();

    // print to terminal
    // p.color_mode  = backward::ColorMode::never;      // no color in file
    p.print(st, stderr);

    // Reset the signal handler to default and re-raise the signal
    std::signal(signum, SIG_DFL);
    raise(signum);
}


}   // namespace uOS
