#include "data/indexer/TaskBuildIndex.h"

#include "utility/AppPath.h"
#include "utility/logging/FileLogger.h"
#include "utility/messaging/type/indexing/MessageIndexingStatus.h"
#include "utility/messaging/type/MessageStatus.h"
#include "utility/scheduling/Blackboard.h"
#include "utility/TimeStamp.h"
#include "utility/UserPaths.h"
#include "utility/utilityApp.h"

#include "component/view/DialogView.h"
#include "data/indexer/IndexerCommandList.h"
#include "data/indexer/interprocess/InterprocessIndexer.h"
#include "data/storage/StorageProvider.h"

#if _WIN32
const std::wstring TaskBuildIndex::s_processName(L"sourcetrail_indexer.exe");
#else
const std::wstring TaskBuildIndex::s_processName(L"sourcetrail_indexer");
#endif

TaskBuildIndex::TaskBuildIndex(
	size_t processCount,
	std::shared_ptr<IndexerCommandList> indexerCommandList,
	std::shared_ptr<StorageProvider> storageProvider,
	std::shared_ptr<DialogView> dialogView,
	const std::string& appUUID,
	bool multiProcessIndexing
)
	: m_indexerCommandList(indexerCommandList)
	, m_storageProvider(storageProvider)
	, m_dialogView(dialogView)
	, m_appUUID(appUUID)
	, m_multiProcessIndexing(multiProcessIndexing)
	, m_interprocessIndexerCommandManager(appUUID, 0, true)
	, m_interprocessIndexingStatusManager(appUUID, 0, true)
	, m_processCount(processCount)
	, m_interrupted(false)
	, m_lastCommandCount(0)
	, m_indexingFileCount(0)
	, m_runningThreadCount(0)
{
}

void TaskBuildIndex::doEnter(std::shared_ptr<Blackboard> blackboard)
{
	m_indexingFileCount = 0;
	updateIndexingDialog(blackboard, std::vector<FilePath>());

	{
		std::lock_guard<std::mutex> lock(blackboard->getMutex());
		blackboard->set("indexer_count", (int)m_processCount);
	}

	// move indexer commands to shared memory
	m_lastCommandCount = m_indexerCommandList->size();
	m_interprocessIndexerCommandManager.setIndexerCommands(m_indexerCommandList->getAllCommands());

	std::wstring logFilePath;
	Logger* logger = LogManager::getInstance()->getLoggerByType("FileLogger");
	if (logger)
	{
		logFilePath = dynamic_cast<FileLogger*>(logger)->getLogFilePath().wstr();
	}

	// start indexer processes
	for (unsigned int i = 0; i < m_processCount; i++)
	{
		const int processId = i + 1; // 0 remains reserved for the main process

		m_interprocessIntermediateStorageManagers.push_back(
			std::make_shared<InterprocessIntermediateStorageManager>(m_appUUID, processId, true)
		);

		if (m_multiProcessIndexing)
		{
			m_processThreads.push_back(new std::thread(&TaskBuildIndex::runIndexerProcess, this, processId, logFilePath));
		}
		else
		{
			m_processThreads.push_back(new std::thread(&TaskBuildIndex::runIndexerThread, this, processId));
		}
	}
}

Task::TaskState TaskBuildIndex::doUpdate(std::shared_ptr<Blackboard> blackboard)
{
	size_t runningThreadCount = 0;
	{
		std::lock_guard<std::mutex> lock(m_runningThreadCountMutex);
		runningThreadCount = m_runningThreadCount;
	}

	size_t commandCount = m_interprocessIndexerCommandManager.indexerCommandCount();
	if (commandCount != m_lastCommandCount)
	{
		std::vector<FilePath> indexingFiles = m_interprocessIndexingStatusManager.getCurrentlyIndexedSourceFilePaths();
		if (indexingFiles.size())
		{
			updateIndexingDialog(blackboard, indexingFiles);
		}

		m_lastCommandCount = commandCount;
	}

	if (commandCount == 0 && runningThreadCount == 0)
	{
		return STATE_FAILURE;
	}
	else if (m_interrupted)
	{
		std::lock_guard<std::mutex> lock(blackboard->getMutex());
		blackboard->set("interrupted_indexing", true);

		// clear indexer commands, this causes the indexer processes to return when finished with respective current indexer commands
		m_interprocessIndexerCommandManager.clearIndexerCommands();
		return STATE_FAILURE;
	}

	if (fetchIntermediateStorages(blackboard))
	{
		updateIndexingDialog(blackboard, std::vector<FilePath>());
	}

	const int SLEEP_TIME_MS = 50;
	std::this_thread::sleep_for(std::chrono::milliseconds(SLEEP_TIME_MS));

	return STATE_RUNNING;
}

void TaskBuildIndex::doExit(std::shared_ptr<Blackboard> blackboard)
{
	for (auto processThread : m_processThreads)
	{
		processThread->join();
		delete processThread;
	}
	m_processThreads.clear();

	while (fetchIntermediateStorages(blackboard));

	std::vector<FilePath> crashedFiles = m_interprocessIndexingStatusManager.getCrashedSourceFilePaths();
	if (crashedFiles.size())
	{
		std::shared_ptr<IntermediateStorage> is = std::make_shared<IntermediateStorage>();
		for (const FilePath& path : crashedFiles)
		{
			is->addError(StorageErrorData(
				L"The translation unit threw an exception during indexing. Please check if the source file "
				"conforms to the specified language standard and all necessary options are defined within your project "
				"setup.", path.wstr(), 1, 1, path.wstr(), true, true
			));
			LOG_INFO(L"crashed translation unit: " + path.wstr());
		}
		m_storageProvider->insert(is);
	}

	std::lock_guard<std::mutex> lock(blackboard->getMutex());
	blackboard->set("indexer_count", 0);
}

void TaskBuildIndex::doReset(std::shared_ptr<Blackboard> blackboard)
{
}

void TaskBuildIndex::terminate()
{
	m_interrupted = true;
	utility::killRunningProcesses();
}

void TaskBuildIndex::handleMessage(MessageInterruptTasks* message)
{
	if (!m_dialogView->dialogsHidden())
	{
		m_interrupted = true;
	}
}

void TaskBuildIndex::runIndexerProcess(int processId, const std::wstring& logFilePath)
{
	{
		std::lock_guard<std::mutex> lock(m_runningThreadCountMutex);
		m_runningThreadCount++;
	}

	FilePath indexerProcessPath = AppPath::getAppPath().concatenate(s_processName);
	if (!indexerProcessPath.exists())
	{
		m_interrupted = true;
		LOG_ERROR("Cannot start indexer process because executable is missing at \"" + indexerProcessPath.str() + "\"");
		return;
	}

	const std::wstring commandPath = L"\"" + indexerProcessPath.wstr() + L"\"";
	std::vector<std::wstring> commandArguments;
	commandArguments.push_back(std::to_wstring(processId));
	commandArguments.push_back(utility::decodeFromUtf8(m_appUUID));
	commandArguments.push_back(L"\"" + AppPath::getAppPath().getAbsolute().wstr() + L"\"");
	commandArguments.push_back(L"\"" + UserPaths::getUserDataPath().getAbsolute().wstr() + L"\"");

	if (!logFilePath.empty())
	{
		commandArguments.push_back(L"\"" + logFilePath + L"\"");
	}

	int result = 1;
	while (result != 0 && !m_interrupted)
	{
		result = utility::executeProcessAndGetExitCode(commandPath, commandArguments, FilePath(), -1);

		LOG_INFO_STREAM(<< "Indexer process " << processId << " returned with " + std::to_string(result));
	}

	{
		std::lock_guard<std::mutex> lock(m_runningThreadCountMutex);
		m_runningThreadCount--;
	}
}

void TaskBuildIndex::runIndexerThread(int processId)
{
	{
		std::lock_guard<std::mutex> lock(m_runningThreadCountMutex);
		m_runningThreadCount++;
	}

	InterprocessIndexer indexer(m_appUUID, processId);
	indexer.work();

	{
		std::lock_guard<std::mutex> lock(m_runningThreadCountMutex);
		m_runningThreadCount--;
	}
}

bool TaskBuildIndex::fetchIntermediateStorages(std::shared_ptr<Blackboard> blackboard)
{
	int poppedStorageCount = 0;

	int providerStorageCount = m_storageProvider->getStorageCount();
	if (providerStorageCount > 10)
	{
		LOG_INFO_STREAM(<< "waiting, too many storages queued: " << providerStorageCount);

		const int SLEEP_TIME_MS = 100;
		std::this_thread::sleep_for(std::chrono::milliseconds(SLEEP_TIME_MS));

		return true;
	}

	TimeStamp t = TimeStamp::now();
	do
	{
		Id finishedProcessId = m_interprocessIndexingStatusManager.getNextFinishedProcessId();
		if (!finishedProcessId || finishedProcessId > m_interprocessIntermediateStorageManagers.size())
		{
			break;
		}

		std::shared_ptr<InterprocessIntermediateStorageManager> storageManager =
			m_interprocessIntermediateStorageManagers[finishedProcessId - 1];

		int storageCount = storageManager->getIntermediateStorageCount();
		if (!storageCount)
		{
			break;
		}

		LOG_INFO_STREAM(<< storageManager->getProcessId() << " - storage count: " << storageCount);
		m_storageProvider->insert(storageManager->popIntermediateStorage());
		poppedStorageCount++;
	}
	while (TimeStamp::now().deltaMS(t) < 500); // don't process all storages at once to allow for status updates in-between

	if (poppedStorageCount > 0)
	{
		std::lock_guard<std::mutex> lock(blackboard->getMutex());

		int indexedSourceFileCount = 0;
		blackboard->get("indexed_source_file_count", indexedSourceFileCount);
		blackboard->set("indexed_source_file_count", indexedSourceFileCount + poppedStorageCount);
		return true;
	}

	return false;
}

void TaskBuildIndex::updateIndexingDialog(
	std::shared_ptr<Blackboard> blackboard, const std::vector<FilePath>& sourcePaths)
{
	// TODO: factor in unindexed files...
	int sourceFileCount = 0;
	int indexedSourceFileCount = 0;
	{
		std::lock_guard<std::mutex> lock(blackboard->getMutex());
		blackboard->get("source_file_count", sourceFileCount);
		blackboard->get("indexed_source_file_count", indexedSourceFileCount);
	}

	m_indexingFileCount += sourcePaths.size();

	m_dialogView->updateIndexingDialog(m_indexingFileCount, indexedSourceFileCount, sourceFileCount, sourcePaths);

	int progress = 0;
	if (sourceFileCount)
	{
		progress = indexedSourceFileCount * 100 / sourceFileCount;
	}
	MessageIndexingStatus(true, progress).dispatch();
}
