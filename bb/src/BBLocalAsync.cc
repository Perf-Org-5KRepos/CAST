/*******************************************************************************
 |    BBLocalAsync.cc
 |
 |  © Copyright IBM Corporation 2015,2016. All Rights Reserved
 |
 |    This program is licensed under the terms of the Eclipse Public License
 |    v1.0 as published by the Eclipse Foundation and available at
 |    http://www.eclipse.org/legal/epl-v10.html
 |
 |    U.S. Government Users Restricted Rights:  Use, duplication or disclosure
 |    restricted by GSA ADP Schedule Contract with IBM Corp.
 *******************************************************************************/

#include "bbcounters.h"
#include "bbinternal.h"
#include "BBLocalAsync.h"
#include "BBLV_Info.h"
#include "BBLV_Metadata.h"
#include "BBTagInfoMap.h"
#include "usage.h"
#include "xfer.h"

#include <boost/filesystem.hpp>

using namespace std;
using namespace boost;
namespace bfs = boost::filesystem;


/*
 * Global data
 */
map<std::string, std::string> diskStatCache;

unsigned long bbcounters_shadow[BB_COUNTER_MAX];

atomic<int64_t> g_Last_Port_Rcv_Data_Delta(-1);
atomic<int64_t> g_Last_Port_Xmit_Data_Delta(-1);
string g_IB_Adapter = "mlx5_0";


/*
 * Helper methods
 */
std::string getLocalAsyncPriorityStr(LOCAL_ASYNC_REQUEST_PRIORITY pPriority)
{
    std::string l_Priority;
    switch (pPriority)
    {
        case NONE:
            l_Priority = "NONE";
            break;

        case HIGH:
            l_Priority = "HIGH";
            break;

        case MEDIUM_HIGH:
            l_Priority = "MEDIUM_HIGH";
            break;

        case MEDIUM:
            l_Priority = "MEDIUM";
            break;

        case MEDIUM_LOW:
            l_Priority = "MEDIUM_LOW";
            break;

        case LOW:
            l_Priority = "LOW";
            break;

        case VERY_LOW:
            l_Priority = "VERY_LOW";
            break;

        default:
            l_Priority = "Unknown";
            break;
    }

    return l_Priority;
}

int highIB_Activity()
{
    int rc = 0;

    if (g_Last_Port_Rcv_Data_Delta > 0)
    {
        if (g_Last_Port_Rcv_Data_Delta < g_IBStatsLowActivityClipValue)
        {
            if (g_Last_Port_Xmit_Data_Delta > 0)
            {
                if (g_Last_Port_Xmit_Data_Delta >= g_IBStatsLowActivityClipValue)
                {
                    rc = 1;
                }
            }
            else
            {
                // Undetermined IB activity
                rc = -1;
            }
        }
        else
        {
            // High IB activity
            rc = 1;
        }
    }
    else
    {
        // Undetermined IB activity
        rc = -1;
    }

    return rc;
}


/*
 * BBAsyncRequestData class methods
 */
int64_t BBAsyncRequestData::addRequest(BBLocalRequest* pRequest)
{
    int64_t rc = -1;

    lock();
    try
    {
        requests.push(pRequest);
        rc = increment(lastRequestNumberIssued);
        if (pRequest->dumpOnAdd())
        {
            pRequest->dump("BBLocalAsync::addRequest(): Enqueuing: ");
        }
    }
    catch (ExceptionBailout& e) { }
    catch (std::exception& e)
    {
        rc = -1;
        LOG_ERROR_RC_WITH_EXCEPTION(__FILE__, __FUNCTION__, __LINE__, e, rc);
    }
    unlock();

    return rc;
}

void BBAsyncRequestData::dump(const char* pPrefix)
{
    LOG(bb,info) << "  Priority " << setw(11) << getLocalAsyncPriorityStr(priority) << ": last issued " << lastRequestNumberIssued \
                 << ", last dispatched " << lastRequestNumberDispatched << ", last processed " << lastRequestNumberProcessed \
                 << ", max concurrent " << maximumConcurrentRunning<< ", # OutOfSeq " << size(outOfSequenceRequests);

    return;
}

int64_t BBAsyncRequestData::getNumberOfInFlightRequests()
{
    int64_t l_NumberOfRequests = 0;

    lock();
    l_NumberOfRequests = lastRequestNumberDispatched - lastRequestNumberProcessed;
    unlock();

    return l_NumberOfRequests;
}

int64_t BBAsyncRequestData::increment(int64_t& pNumber)
{
    do
    {
        ++pNumber;
    } while (!pNumber);

    return pNumber;
}

size_t BBAsyncRequestData::numberOfNonDispatchedRequests()
{
    size_t l_NumberOfRequests = 0;

    lock();
    l_NumberOfRequests = requests.size();
    unlock();

    return l_NumberOfRequests;
}

void BBAsyncRequestData::recordRequestCompletion(int64_t pRequestNumber)
{
    lock();
    if (lastRequestNumberProcessed + 1 == pRequestNumber)
    {
        ++lastRequestNumberProcessed;
        if (outOfSequenceRequests.size())
        {
            bool l_AllDone = false;
            while (!l_AllDone)
            {
                l_AllDone = true;
                vector<int64_t>::iterator it;
                for (it = outOfSequenceRequests.begin(); it != outOfSequenceRequests.end(); ++it)
                {
                    if (lastRequestNumberProcessed + 1 == *it)
                    {
                        ++lastRequestNumberProcessed;
                        outOfSequenceRequests.erase(it);
                        l_AllDone = false;
                        LOG(bb,debug) << "BBLocalAsync::recordRequestCompletion(): Priority " << getLocalAsyncPriorityStr(priority) \
                                      << ", removed request number " << *it << " from outOfSequenceRequests";
                        break;
                    }
                }
            }
        }
    }
    else
    {
        outOfSequenceRequests.push_back(pRequestNumber);
        LOG(bb,debug) << "BBLocalAsync::recordRequestCompletion(): Priority " << getLocalAsyncPriorityStr(priority) \
                      << ", pushed request number " << pRequestNumber << " onto outOfSequenceRequests";
    }

    LOG(bb,debug) << "BBLocalAsync::recordRequestCompletion(): Priority " << getLocalAsyncPriorityStr(priority) \
                  << ", completed request, lastRequestNumberProcessed " << lastRequestNumberProcessed;

    unlock();

    return;
}

int64_t BBAsyncRequestData::removeNextRequest(BBLocalRequest* &pRequest)
{
    int64_t rc = -1;

    lock();
    try
    {
        pRequest = requests.front();
        if (pRequest)
        {
            requests.pop();
            rc = increment(lastRequestNumberDispatched);
            if (pRequest->dumpOnRemove())
            {
                pRequest->dump("BBLocalAsync::removeNextRequest(): Dequeuing: ");
            }
        }
    }
    catch (ExceptionBailout& e) { }
    catch (std::exception& e)
    {
        rc = -1;
        LOG_ERROR_RC_WITH_EXCEPTION(__FILE__, __FUNCTION__, __LINE__, e, rc);
    }
    unlock();

    return rc;
}


/*
 * BBLocalRequest class methods
 */
void BBLocalRequest::dumpRequest(stringstream& pStream)
{
    pStream << "Name " << name << ", priority " << getLocalAsyncPriorityStr(priority);

    return;
}


/*
 * BBLocalAsync class methods
 */
void* BBLocalAsync::asyncRequestWorker(void* ptr)
{
    BBLocalRequest* l_Request;

    while (1)
    {
        l_Request = 0;
        int64_t l_RequestNumber = 0;

        try
        {
            verifyInitLockState();

            l_RequestNumber = g_LocalAsync.getNextRequest(l_Request);
            if (l_RequestNumber > 0 && l_Request)
            {
                l_Request->doit();
            }
        }
        catch (ExceptionBailout& e) { }
        catch (std::exception& e)
        {
            LOG_ERROR_WITH_EXCEPTION(__FILE__, __FUNCTION__, __LINE__, e);
        }

        if (l_Request)
        {
            try
            {
                g_LocalAsync.recordRequestCompletion(l_RequestNumber, l_Request);
            }
            catch (std::exception& e)
            {
                // Tolerate everything
            }
            try
            {
                if (l_Request->dumpOnDelete())
                {
                    l_Request->dump("BBLocalAsync::asyncRequestWorker(): Deleting: ");
                }
            }
            catch (std::exception& e)
            {
                // Tolerate everything
            }
            delete l_Request;
            l_Request = 0;
        }
    }

    return NULL;
}

int BBLocalAsync::dispatchFromThisQueue(LOCAL_ASYNC_REQUEST_PRIORITY pPriority)
{
    int rc = 0;

    int64_t l_NumberOfNonDispatchedRequests = requestData[pPriority]->numberOfNonDispatchedRequests();
    int64_t l_MaximumConcurrentRunning = 0;
    int64_t l_NumberOfInFlightRequests = 0;

    if (l_NumberOfNonDispatchedRequests)
    {
        l_MaximumConcurrentRunning = (int64_t)requestData[pPriority]->getMaximumConcurrentRunning();
        if (l_MaximumConcurrentRunning)
        {
            l_NumberOfInFlightRequests = (int64_t)requestData[pPriority]->getNumberOfInFlightRequests();
            if (l_NumberOfInFlightRequests < l_MaximumConcurrentRunning)
            {
                rc = 1;
            }
        }
        else
        {
            rc = 1;
        }
    }

    if (rc == 1)
    {
        LOG(bb,debug) << "BBLocalAsync::dispatchFromThisQueue(): ==> Scheduling async request having priority " << getLocalAsyncPriorityStr(pPriority) \
                      << ", NumberOfNonDispatchedRequests " << l_NumberOfNonDispatchedRequests \
                      << ", NumberOfInFlightRequests " << l_NumberOfInFlightRequests \
                      << ", MaximumConcurrentRunning " << l_MaximumConcurrentRunning;
    }

    return rc;
}

void BBLocalAsync::dump(const char* pPrefix)
{
    lock();
    try
    {
        LOG(bb,info) << ">>>>> Start: Local Async Mgr <<<<<";

        for (map<LOCAL_ASYNC_REQUEST_PRIORITY, BBAsyncRequestData*>::iterator rd = requestData.begin(); rd != requestData.end(); ++rd)
        {
            rd->second->dump();
        }

        LOG(bb,info) << ">>>>>   End: Local Async Mgr <<<<<";
    }
    catch (ExceptionBailout& e) { }
    catch (std::exception& e)
    {
        LOG_ERROR_WITH_EXCEPTION(__FILE__, __FUNCTION__, __LINE__, e);
    }
    unlock();

    return;
}

int64_t BBLocalAsync::getLastRequestNumberProcessed(BBLocalRequest* pRequest)
{
    int64_t l_RequestNumber = -1;

    if (pRequest)
    {
        l_RequestNumber = requestData[pRequest->getPriority()]->getLastRequestNumberProcessed();
    }

    return l_RequestNumber;
}

int64_t BBLocalAsync::getNextRequest(BBLocalRequest* &pRequest)
{
    int64_t rc = -1;

    sem_wait(&work);

    lock();
    try
    {
        pRequest = (BBLocalRequest*)0;
        LOCAL_ASYNC_REQUEST_PRIORITY l_Priority = NONE;
        vector<BBAsyncRequestType>::iterator it;
        for (it = requestType.begin(); it != requestType.end(); ++it)
        {
            if (dispatchFromThisQueue(it->priority))
            {
                l_Priority = it->priority;
                break;
            }
        }

        if (l_Priority != NONE)
        {
            rc = requestData[l_Priority]->removeNextRequest(pRequest);
        }

        if (rc < 0 || l_Priority == NONE)
        {
            // Cannot dispatch new work...  Delay 250 milliseconds and retry...
            LOG(bb,debug) << "BBLocalAsync::getNextRequest(): Cannot dispatch new work...";
            unlock();
            usleep(250000);
            lock();
            sem_post(&work);
        }
    }
    catch (ExceptionBailout& e) { }
    catch (std::exception& e)
    {
        rc = -1;
        LOG_ERROR_RC_WITH_EXCEPTION(__FILE__, __FUNCTION__, __LINE__, e, rc);
    }
    unlock();

    return rc;
}

int BBLocalAsync::init()
{
    int rc = 0;
    stringstream errorText;

    try
    {
        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

        LOG(bb,info) << "Starting " << g_NumberOfAsyncRequestsThreads << " async request threads";

        for (unsigned int i=0; i<g_NumberOfAsyncRequestsThreads; i++)
        {
            rc = pthread_create(&tid, &attr, BBLocalAsync::asyncRequestWorker, NULL);
            if (rc)
            {
                errorText << "Error occurred in BBLocalAsync::init()";
                bberror << err("error.numthreads", g_NumberOfAsyncRequestsThreads);
                LOG_ERROR_TEXT_RC_RAS_AND_BAIL(errorText, rc, bb.lar.pthread_create);
            }
        }

        // Setup the request data for each supported priority...
        vector<BBAsyncRequestType>::iterator it;
        for (it = requestType.begin(); it != requestType.end(); ++it)
        {
            requestData[it->priority] = new BBAsyncRequestData(it->priority, round(g_NumberOfAsyncRequestsThreads*(it->percentage_of_threads)));
        }
    }
    catch (ExceptionBailout& e) { }
    catch (std::exception& e)
    {
        rc = -1;
        LOG_ERROR_RC_WITH_EXCEPTION(__FILE__, __FUNCTION__, __LINE__, e, rc);
    }

    return rc;
}

// NOTE pRequest must be allocated out of heap.
int64_t BBLocalAsync::issueAsyncRequest(BBLocalRequest* pRequest)
{
    int64_t rc = -1;

    lock();
    try
    {
        // NOTE: Once the request has been added, the BBLocalAsync object
        //       owns the heap that was allocated for the request.
        //       BBLocalAsync will delete this space when the request is complete.
        rc = requestData[pRequest->getPriority()]->addRequest(pRequest);
        if (rc > 0)
        {
            sem_post(&work);
        }
    }
    catch (ExceptionBailout& e) { }
    catch (std::exception& e)
    {
        rc = -1;
        LOG_ERROR_RC_WITH_EXCEPTION(__FILE__, __FUNCTION__, __LINE__, e, rc);
    }
    unlock();

    return rc;
}

void BBLocalAsync::recordRequestCompletion(int64_t l_RequestNumber, BBLocalRequest* l_Request)
{
    requestData[l_Request->getPriority()]->recordRequestCompletion(l_RequestNumber);
    LOG(bb,debug) << "BBLocalAsync::recordRequestCompletion(): Completed local async request having priority " \
                  << getLocalAsyncPriorityStr(l_Request->getPriority()) \
                  << ", NumberOfNonDispatchedRequests " << requestData[l_Request->getPriority()]->numberOfNonDispatchedRequests() \
                  << ", NumberOfInFlightRequests " << requestData[l_Request->getPriority()]->getNumberOfInFlightRequests() \
                  << ", MaximumConcurrentRunning " << requestData[l_Request->getPriority()]->getMaximumConcurrentRunning();

    return;
}


/*
 * doit() methods
 */
void BBAsyncRemoveJobInfo::doit()
{
    const uint64_t l_JobsToSchedulePerPass = g_AsyncRemoveJobInfoNumberPerGroup;
    uint64_t l_JobsScheduled = 0;

    vector<string> l_PathJobIds;
    l_PathJobIds.reserve(100);
    try
    {
        bool l_AllDone = false;
        while (!l_AllDone)
        {
            l_AllDone = true;
            int rc = HandleFile::get_xbbServerGetCurrentJobIds(l_PathJobIds, ONLY_RETURN_REMOVED_JOBIDS);
            if ((!rc) && l_PathJobIds.size() > 0)
            {
                for (size_t i=0; i<l_PathJobIds.size() && (l_JobsToSchedulePerPass == 0 || l_JobsScheduled < l_JobsToSchedulePerPass); i++)
                {
                    bfs::path job = bfs::path(l_PathJobIds[i]);
                    bfs::path l_PathToRemove = job.parent_path().string() + "/." + job.filename().string();
                    LOG(bb,debug) << "asyncRemoveJobInfo(): " << job.string() << " being renamed to " << l_PathToRemove.string();
                    try
                    {
                        bfs::rename(job, l_PathToRemove);
                    }
                    catch (std::exception& e1)
                    {
                        rc = -1;
                    }

                    if (!rc)
                    {
                        BBPruneMetadata* l_Request = new BBPruneMetadata(l_PathToRemove.string());
                        g_LocalAsync.issueAsyncRequest(l_Request);
                        ++l_JobsScheduled;
                    }
                    rc = 0;
                }
                if (l_JobsToSchedulePerPass > 0 && l_JobsScheduled >= l_JobsToSchedulePerPass)
                {
                    // In an attempt to let other servers jump in and schedule some pruning of the metadata...
                    // NOTE: We may delay for 1 minute when there are no jobs left to schedule, but the
                    //       timing here isn't that critical...
                    usleep(60000000);    // Delay for 1 minute...
                    l_JobsScheduled = 0;
                    l_AllDone = false;
                }
            }
        }
    }
    catch (ExceptionBailout& e) { }
    catch (std::exception& e)
    {
        LOG_ERROR_WITH_EXCEPTION(__FILE__, __FUNCTION__, __LINE__, e);
    }

    int l_WorkQueueMgrLocked = 0;
    try
    {
        wrkqmgr.lockWorkQueueMgr((LVKey*)0, "BBAsyncRemoveJobInfo::doit()");
        l_WorkQueueMgrLocked = 1;

        wrkqmgr.setAsyncRmvJobInfoTimerFired(0);
        wrkqmgr.setAsyncRmvJobInfoTimerCount(0);

        l_WorkQueueMgrLocked = 0;
        wrkqmgr.unlockWorkQueueMgr((LVKey*)0, "BBAsyncRemoveJobInfo::doit()");
    }
    catch(ExceptionBailout& e) { }
    catch(std::exception& e)
    {
        LOG_ERROR_WITH_EXCEPTION(__FILE__, __FUNCTION__, __LINE__, e);
    }

    if (l_WorkQueueMgrLocked)
    {
        l_WorkQueueMgrLocked = 0;
        wrkqmgr.unlockWorkQueueMgr((LVKey*)0, "BBAsyncRemoveJobInfo::doit() - On exit");
    }

    return;
}

void BBCheckCycleActivities::doit()
{
    int l_WorkQueueMgrLocked = 0;
    try
    {
        wrkqmgr.lockWorkQueueMgr((LVKey*)0, "BBCheckCycleActivities::doit()");
        l_WorkQueueMgrLocked = 1;

        // See if it is time to have a heartbeat for this bbServer
        if (wrkqmgr.timeForServerHeartbeat())
        {
            // Tell the world this bbServer is still alive...
            char l_AsyncCmd[AsyncRequest::MAX_DATA_LENGTH] = {'\0'};
            string l_CurrentTime = HeartbeatEntry::getHeartbeatCurrentTimeStr();
            snprintf(l_AsyncCmd, sizeof(l_AsyncCmd), "heartbeat 0 0 0 0 0 None %s", l_CurrentTime.c_str());
            AsyncRequest l_Request = AsyncRequest(l_AsyncCmd);
            wrkqmgr.appendAsyncRequest(l_Request);
        }

        // See if it is time to dump local async manager
        if (wrkqmgr.timeToPerformLocalAsyncDump())
        {
            BBDumpLocalAsync* l_Request = new BBDumpLocalAsync();
            g_LocalAsync.issueAsyncRequest(l_Request);
            wrkqmgr.setDumpLocalAsyncTimerFired(1);
        }

        // See if it is time to dump IB Stats
        if (wrkqmgr.timeToPerformIBStatsDump())
        {
            BBIB_Stats* l_Request = new BBIB_Stats();
            g_LocalAsync.issueAsyncRequest(l_Request);
            wrkqmgr.setIBStatsTimerFired(1);
        }

        // See if it is time to dump IO Stats
        if (wrkqmgr.timeToPerformIOStatsDump())
        {
            BBIO_Stats* l_Request = new BBIO_Stats();
            g_LocalAsync.issueAsyncRequest(l_Request);
            wrkqmgr.setIOStatsTimerFired(1);
        }

        // See if it is time to dump counters
        if (wrkqmgr.timeToPerformCountersDump())
        {
            BBCounters* l_Request = new BBCounters();
            g_LocalAsync.issueAsyncRequest(l_Request);
            wrkqmgr.setDumpCountersTimerFired(1);
        }

        // See if it is time to asynchronously remove job information from the cross-bbServer metadata
        if (g_AsyncRemoveJobInfo)
        {
            if (wrkqmgr.timeToPerformAsyncJobInfoRemoval())
            {
                BBAsyncRemoveJobInfo* l_Request = new BBAsyncRemoveJobInfo();
                g_LocalAsync.issueAsyncRequest(l_Request);
                wrkqmgr.setAsyncRmvJobInfoTimerFired(1);
            }
        }

        // See if it is time to dump the work manager
        if (wrkqmgr.timeToPerformWrkQMgrDump())
        {
            BBDumpWrkQMgr* l_Request = new BBDumpWrkQMgr();
            g_LocalAsync.issueAsyncRequest(l_Request);
            wrkqmgr.setDumpWrkQueueMgrTimerFired(1);
        }

        // See if it is time to dump the heartbeat information
        if (wrkqmgr.timeToPerformHeartbeatDump())
        {
            BBDumpHeartbeatData* l_Request = new BBDumpHeartbeatData();
            g_LocalAsync.issueAsyncRequest(l_Request);
            wrkqmgr.setDumpHeartbeatDataTimerFired(1);
        }

        wrkqmgr.setCycleActivitiesTimerFired(0);

        l_WorkQueueMgrLocked = 0;
        wrkqmgr.unlockWorkQueueMgr((LVKey*)0, "BBCheckCycleActivities::doit()");
    }
    catch(ExceptionBailout& e) { }
    catch(std::exception& e)
    {
        LOG_ERROR_WITH_EXCEPTION(__FILE__, __FUNCTION__, __LINE__, e);
    }

    if (l_WorkQueueMgrLocked)
    {
        l_WorkQueueMgrLocked = 0;
        wrkqmgr.unlockWorkQueueMgr((LVKey*)0, "BBCheckCycleActivities::doit() - On exit");
    }

    return;
}

void BBCleanUpContribId::doit()
{
    WRKQE* l_WrkQE = 0;

    int l_WorkQueueMgrLocked = 0;
    int l_LocalMetadataLocked = 0;
    int l_TransferQueueLocked = 0;

    bool l_AllDone = false;
    try
    {
        while (!l_AllDone)
        {
            l_AllDone = true;
            uint64_t l_JobId = tagid.getJobId();
            wrkqmgr.lockWorkQueueMgr(&lvkey, "BBCleanUpContribId::doit");
            l_WorkQueueMgrLocked = 1;

            int rc = wrkqmgr.getWrkQE(&lvkey, l_WrkQE);
            if (rc == 1 && l_WrkQE)
            {
                rc = 0;
                CurrentWrkQE = l_WrkQE;
                lockTransferQueue(&lvkey, "BBCleanUpContribId::doit");
                l_TransferQueueLocked = 1;
                wrkqmgr.unlockWorkQueueMgr(&lvkey, "BBCleanUpContribId::doit");
                l_WorkQueueMgrLocked = 0;

                size_t l_CurrentNumberOfInFlightExtents = 1;
                {

                    BBLV_Info* l_LV_Info = metadata.getLV_Info(&lvkey);
                    if (l_LV_Info)
                    {
                        l_CurrentNumberOfInFlightExtents = l_LV_Info->moreExtentsToTransfer(handle, contribid, 0);
                    }
                    else
                    {
                        BAIL;
                    }
                }
                l_TransferQueueLocked = 0;
                unlockTransferQueue(&lvkey, "BBCleanUpContribId::doit");

                if (!l_CurrentNumberOfInFlightExtents)
                {
                    lockLocalMetadata(&lvkey, "BBCleanUpContribId::doit");
                    l_LocalMetadataLocked = 1;
                    for (auto it = metadata.metaDataMap.begin(); it != metadata.metaDataMap.end(); ++it)
                    {
                        if ((it->first).first == connection_name && (it->second).getJobId() == l_JobId)
                        {
                            (it->second).getTagInfoMap()->cleanUpContribId(&lvkey, tagid, handle, contribid);
                        }
                    }
                    l_LocalMetadataLocked = 0;
                    unlockLocalMetadata(&lvkey, "BBCleanUpContribId::doit");
                }
                else
                {
                    l_AllDone = false;
                }
            }

            if (!l_AllDone)
            {
                usleep((useconds_t)250000); // Still had extents not yet removed from
                                            // the in-flight queue.  Delay 250 milliseconds.
            }
        }
    }
    catch (ExceptionBailout& e) { }
    catch (std::exception& e)
    {
        LOG_ERROR_WITH_EXCEPTION(__FILE__, __FUNCTION__, __LINE__, e);
    }

    if (l_LocalMetadataLocked)
    {
        l_LocalMetadataLocked = 0;
        unlockLocalMetadata(&lvkey, "BBCleanUpContribId::doit - On exit");
    }

    if (l_TransferQueueLocked)
    {
        l_TransferQueueLocked = 0;
        unlockTransferQueue(&lvkey, "BBCleanUpContribId::doit - On exit");
    }
    CurrentWrkQE = (WRKQE*)0;

    if (l_WorkQueueMgrLocked)
    {
        l_WorkQueueMgrLocked = 0;
        wrkqmgr.unlockWorkQueueMgr(&lvkey, "BBCleanUpContribId::doit - On exit");
    }

    return;
}

void BBCleanUpTagInfo::doit()
{
    WRKQE* l_WrkQE = 0;

    int l_WorkQueueMgrLocked = 0;
    int l_LocalMetadataLocked = 0;
    int l_TransferQueueLocked = 0;

    bool l_AllDone = false;
    try
    {
        while (!l_AllDone)
        {
            l_AllDone = true;
            uint64_t l_JobId = tagid.getJobId();
            wrkqmgr.lockWorkQueueMgr(&lvkey, "BBCleanUpTagInfo::doit");
            l_WorkQueueMgrLocked = 1;

            int rc = wrkqmgr.getWrkQE(&lvkey, l_WrkQE);
            if (rc == 1 && l_WrkQE)
            {
                rc = 0;
                CurrentWrkQE = l_WrkQE;
                lockTransferQueue(&lvkey, "BBCleanUpTagInfo::doit");
                l_TransferQueueLocked = 1;
                wrkqmgr.unlockWorkQueueMgr(&lvkey, "BBCleanUpTagInfo::doit");
                l_WorkQueueMgrLocked = 0;

                size_t l_CurrentNumberOfInFlightExtents = 1;
                {
                    BBLV_Info* l_LV_Info = metadata.getLV_Info(&lvkey);
                    if (l_LV_Info)
                    {
                        BBTagInfo* l_TagInfo = l_LV_Info->getTagInfo(tagid);
                        if (l_TagInfo)
                        {
                            l_CurrentNumberOfInFlightExtents = l_LV_Info->moreExtentsToTransfer(l_TagInfo->getTransferHandle(), UNDEFINED_CONTRIBID, 0);
                        }
                        else
                        {
                            BAIL;
                        }
                    }
                    else
                    {
                        BAIL;
                    }
                }
                l_TransferQueueLocked = 0;
                unlockTransferQueue(&lvkey, "BBCleanUpTagInfo::doit");

                if (!l_CurrentNumberOfInFlightExtents)
                {
                    lockLocalMetadata(&lvkey, "BBCleanUpTagInfo::doit");
                    l_LocalMetadataLocked = 1;
                    for (auto it = metadata.metaDataMap.begin(); it != metadata.metaDataMap.end(); ++it)
                    {
                        if ((it->first).first == connection_name && (it->second).getJobId() == l_JobId)
                        {
                            (it->second).getTagInfoMap()->cleanUpTagInfo(&lvkey, tagid);
                        }
                    }
                    l_LocalMetadataLocked = 0;
                    unlockLocalMetadata(&lvkey, "BBCleanUpTagInfo::doit");
                }
                else
                {
                    l_AllDone = false;
                }
            }

            if (!l_AllDone)
            {
                usleep((useconds_t)250000); // Still had extents not yet removed from
                                            // the in-flight queue.  Delay 250 milliseconds.
            }
        }
    }
    catch (ExceptionBailout& e) { }
    catch (std::exception& e)
    {
        LOG_ERROR_WITH_EXCEPTION(__FILE__, __FUNCTION__, __LINE__, e);
    }

    if (l_LocalMetadataLocked)
    {
        l_LocalMetadataLocked = 0;
        unlockLocalMetadata(&lvkey, "BBCleanUpTagInfo::doit - On exit");
    }

    if (l_TransferQueueLocked)
    {
        l_TransferQueueLocked = 0;
        unlockTransferQueue(&lvkey, "BBCleanUpTagInfo::doit - On exit");
    }
    CurrentWrkQE = (WRKQE*)0;

    if (l_WorkQueueMgrLocked)
    {
        l_WorkQueueMgrLocked = 0;
        wrkqmgr.unlockWorkQueueMgr(&lvkey, "BBCleanUpTagInfo::doit - On exit");
    }

    return;
}

void BBCounters::doit()
{
    try
    {
        #define MKBBCOUNTER(id) if(bbcounters[BB_COUNTERS_##id] != bbcounters_shadow[BB_COUNTERS_##id]) { LOG(bb,always) << "BB Counter '" #id "' = " << bbcounters[BB_COUNTERS_##id] << " (delta " << (bbcounters[BB_COUNTERS_##id] - bbcounters_shadow[BB_COUNTERS_##id]) << ")"; bbcounters_shadow[BB_COUNTERS_##id] = bbcounters[BB_COUNTERS_##id]; }
        #include "bbcounters.h"
    }
    catch (ExceptionBailout& e) { }
    catch (std::exception& e)
    {
        LOG_ERROR_WITH_EXCEPTION(__FILE__, __FUNCTION__, __LINE__, e);
    }

    int l_WorkQueueMgrLocked = 0;
    try
    {
        wrkqmgr.lockWorkQueueMgr((LVKey*)0, "BBCounters::doit()");
        l_WorkQueueMgrLocked = 1;

        wrkqmgr.setDumpCountersTimerFired(0);
        wrkqmgr.setDumpCountersTimerCount(0);

        l_WorkQueueMgrLocked = 0;
        wrkqmgr.unlockWorkQueueMgr((LVKey*)0, "BBCounters::doit()");
    }
    catch(ExceptionBailout& e) { }
    catch(std::exception& e)
    {
        LOG_ERROR_WITH_EXCEPTION(__FILE__, __FUNCTION__, __LINE__, e);
    }

    if (l_WorkQueueMgrLocked)
    {
        l_WorkQueueMgrLocked = 0;
        wrkqmgr.unlockWorkQueueMgr((LVKey*)0, "BBCounters::doit() - On exit");
    }

    return;
}

void BBDumpHeartbeatData::doit()
{
    int l_WorkQueueMgrLocked = 0;

    try
    {
        wrkqmgr.lockWorkQueueMgr((LVKey*)0, "BBDumpWrkQMgr::doit()");
        l_WorkQueueMgrLocked = 1;

        wrkqmgr.dumpHeartbeatData("info");
        wrkqmgr.setDumpHeartbeatDataTimerFired(0);
        wrkqmgr.setDumpHeartbeatDataTimerCount(0);

        l_WorkQueueMgrLocked = 0;
        wrkqmgr.unlockWorkQueueMgr((LVKey*)0, "BBDumpWrkQMgr::doit()");
    }
    catch (ExceptionBailout& e) { }
    catch (std::exception& e)
    {
        LOG_ERROR_WITH_EXCEPTION(__FILE__, __FUNCTION__, __LINE__, e);
    }

    if (l_WorkQueueMgrLocked)
    {
        l_WorkQueueMgrLocked = 0;
        wrkqmgr.unlockWorkQueueMgr((LVKey*)0, "BBDumpWrkQMgr::doit() - On exit");
    }

    return;
}

void BBDumpLocalAsync::doit()
{
    int l_WorkQueueMgrLocked = 0;

    try
    {
        wrkqmgr.lockWorkQueueMgr((LVKey*)0, "BBDumpLocalAsync::doit()");
        l_WorkQueueMgrLocked = 1;

        g_LocalAsync.dump(" Local Async Mgr (Not an error - Timer Interval)");
        wrkqmgr.setDumpLocalAsyncTimerFired(0);
        wrkqmgr.setDumpLocalAsyncTimerCount(0);

        l_WorkQueueMgrLocked = 0;
        wrkqmgr.unlockWorkQueueMgr((LVKey*)0, "BBDumpLocalAsync::doit()");
    }
    catch (ExceptionBailout& e) { }
    catch (std::exception& e)
    {
        LOG_ERROR_WITH_EXCEPTION(__FILE__, __FUNCTION__, __LINE__, e);
    }

    if (l_WorkQueueMgrLocked)
    {
        l_WorkQueueMgrLocked = 0;
        wrkqmgr.unlockWorkQueueMgr((LVKey*)0, "BBDumpLocalAsync::doit() - On exit");
    }

    return;
}

void BBDumpWrkQMgr::doit()
{
    int l_WorkQueueMgrLocked = 0;

    try
    {
        wrkqmgr.lockWorkQueueMgr((LVKey*)0, "BBDumpWrkQMgr::doit()");
        l_WorkQueueMgrLocked = 1;

        wrkqmgr.dump("info", " Work Queue Mgr (Not an error - Timer Interval)", DUMP_ALWAYS);
        wrkqmgr.setDumpWrkQueueMgrTimerFired(0);
        wrkqmgr.setDumpTimerCount(0);

        l_WorkQueueMgrLocked = 0;
        wrkqmgr.unlockWorkQueueMgr((LVKey*)0, "BBDumpWrkQMgr::doit()");
    }
    catch (ExceptionBailout& e) { }
    catch (std::exception& e)
    {
        LOG_ERROR_WITH_EXCEPTION(__FILE__, __FUNCTION__, __LINE__, e);
    }

    if (l_WorkQueueMgrLocked)
    {
        l_WorkQueueMgrLocked = 0;
        wrkqmgr.unlockWorkQueueMgr((LVKey*)0, "BBDumpWrkQMgr::doit() - On exit");
    }

    return;
}

void BBIB_Stats::doit()
{
    try
    {
        string l_Port_Rcv_Data = "port_rcv_data";
        string l_Port_Xmit_Data = "port_xmit_data";

        for(const auto& line : runCommand("grep '' /sys/class/infiniband/*/ports/*/*counters/*"))
        {
            auto tokens = buildTokens(line, ":");
            if(tokens.size() == 2)
            {
                if(diskStatCache[tokens[0]] != tokens[1])
                {
                    if(!diskStatCache[tokens[0]].empty())
                    {
                        uint64_t l_Delta = stoll(tokens[1]) - stoll(diskStatCache[tokens[0]]);
                        LOG(bb,always) << "IBSTAT:  " << line << " (delta " << l_Delta << ")";
                        if (tokens[0].find(g_IB_Adapter) != string::npos)
                        {
                            if (tokens[0].find(l_Port_Rcv_Data) != string::npos)
                            {
                                g_Last_Port_Rcv_Data_Delta = l_Delta;
                            }
                            else if (tokens[0].find(l_Port_Xmit_Data) != string::npos)
                            {
                                g_Last_Port_Xmit_Data_Delta = l_Delta;
                            }
                        }
                    }
                    else
                    {
                        LOG(bb,always) << "IBSTAT:  " << line;
                    }

                    diskStatCache[tokens[0]] = tokens[1];
                }
            }
        }
    }
    catch(ExceptionBailout& e) { }
    catch(std::exception& e)
    {
        // NOTE:  Set the two static xmit/rcv values to -1 so we DO NOT
        //        attempt any additional async job prunes/removals of branches/trees.
        g_Last_Port_Rcv_Data_Delta = -1;
        g_Last_Port_Xmit_Data_Delta = -1;
        LOG_ERROR_WITH_EXCEPTION(__FILE__, __FUNCTION__, __LINE__, e);
    }

    int l_WorkQueueMgrLocked = 0;
    try
    {
        wrkqmgr.lockWorkQueueMgr((LVKey*)0, "BBIB_Stats::doit()");
        l_WorkQueueMgrLocked = 1;

        wrkqmgr.setIBStatsTimerFired(0);
        wrkqmgr.setIBStatsTimerCount(0);

        l_WorkQueueMgrLocked = 0;
        wrkqmgr.unlockWorkQueueMgr((LVKey*)0, "BBIB_Stats::doit()");
    }
    catch(ExceptionBailout& e) { }
    catch(std::exception& e)
    {
        LOG_ERROR_WITH_EXCEPTION(__FILE__, __FUNCTION__, __LINE__, e);
    }

    if (l_WorkQueueMgrLocked)
    {
        l_WorkQueueMgrLocked = 0;
        wrkqmgr.unlockWorkQueueMgr((LVKey*)0, "BBIB_Stats::doit() - On exit");
    }

    return;
}

void BBIO_Stats::doit()
{
    try
    {
        std::string cmd = string("/usr/bin/timeout ") + to_string(g_DiskStatsRate+2) + string(" /usr/bin/iostat -xym -p ALL ") + to_string(g_DiskStatsRate);

        for(const auto& line : runCommand(cmd))
        {
            auto tokens = buildTokens(line, " ");
            if(tokens.size() > 10)
            {
                if(diskStatCache[tokens[0]] != line)
                {
                    diskStatCache[tokens[0]] = line;
                    LOG(bb,always) << "IOSTAT:  " << line;
                }
            }
        }
    }
    catch (ExceptionBailout& e) { }
    catch (std::exception& e)
    {
        LOG_ERROR_WITH_EXCEPTION(__FILE__, __FUNCTION__, __LINE__, e);
    }

    int l_WorkQueueMgrLocked = 0;
    try
    {
        wrkqmgr.lockWorkQueueMgr((LVKey*)0, "BBIO_Stats::doit()");
        l_WorkQueueMgrLocked = 1;

        wrkqmgr.setIOStatsTimerFired(0);
        wrkqmgr.setIOStatsTimerCount(0);

        l_WorkQueueMgrLocked = 0;
        wrkqmgr.unlockWorkQueueMgr((LVKey*)0, "BBIO_Stats::doit()");
    }
    catch(ExceptionBailout& e) { }
    catch(std::exception& e)
    {
        LOG_ERROR_WITH_EXCEPTION(__FILE__, __FUNCTION__, __LINE__, e);
    }

    if (l_WorkQueueMgrLocked)
    {
        l_WorkQueueMgrLocked = 0;
        wrkqmgr.unlockWorkQueueMgr((LVKey*)0, "BBIO_Stats::doit() - On exit");
    }

    return;
}

void BBLogIt::doit()
{
    try
    {
        LOG(bb,info) << data.c_str();
    }
    catch (ExceptionBailout& e) { }
    catch (std::exception& e)
    {
        LOG_ERROR_WITH_EXCEPTION(__FILE__, __FUNCTION__, __LINE__, e);
    }

    return;
}

void BBPruneMetadata::doit()
{
    int64_t l_RequestNumber = 0;

    bfs::path l_PathToRemove = bfs::path(path);
    try
    {
        vector<std::string> l_HandlesProcessed = vector<std::string>();
        l_HandlesProcessed.reserve(4096);
        bool l_AllDone = false;

        LOG(bb,info) << "BBLocalAsync::BBPruneMetadata(): START: Removal of cross-bbServer metadata at " << path;
        while (!l_AllDone)
        {
            try
            {
                l_AllDone = true;
                if (!access(path.c_str(), F_OK))
                {
                    // Jobid directory path still exists
                    for (auto& jobstep : boost::make_iterator_range(bfs::directory_iterator(l_PathToRemove), {}))
                    {
                        if (!pathIsDirectory(jobstep)) continue;
                        for (auto& handlebucket : boost::make_iterator_range(bfs::directory_iterator(jobstep), {}))
                        {
                            if (!pathIsDirectory(handlebucket)) continue;
                            for (auto& handledir : boost::make_iterator_range(bfs::directory_iterator(handlebucket), {}))
                            {
                                if (!pathIsDirectory(handledir)) continue;
                                std::string l_Handle = handledir.path().string();
                                if (find(l_HandlesProcessed.begin(), l_HandlesProcessed.end(), l_Handle) == l_HandlesProcessed.end())
                                {
                                    BBPruneMetadataBranch* l_Request = new BBPruneMetadataBranch(l_Handle);
                                    l_RequestNumber = g_LocalAsync.issueAsyncRequest(l_Request);
                                    l_HandlesProcessed.push_back(l_Handle);
                                }
                            }
                        }
                    }
                }
            }
            catch (std::exception& e2)
            {
                // Tolerate any exceptions when looping through the jobid directory...
                l_AllDone = false;
            }
        }

        // NOTE: Create a dummy request of the same type as above to test for when that
        //       priority of the requests issued above are complete.  We cannot use the
        //       last request allocated on the heap because once issued, the BBLocalAsync
        //       object owns the existence of that request.  We cannot touch the request
        //       after we issue it.  BBLocalAsync processing will delete the allocated
        //       storage for the request object.
        BBPruneMetadataBranch l_Dummy = BBPruneMetadataBranch("");
        bool l_MsgSent = false;
        while (l_RequestNumber && l_RequestNumber > g_LocalAsync.getLastRequestNumberProcessed(&l_Dummy))
        {
            LOG(bb,debug) << "BBLocalAsync::BBPruneMetadata(): >>>>> DELAY <<<<< l_RequestNumber " << l_RequestNumber \
                          << ", g_LocalAsync.getLastRequestNumberProcessed() " << g_LocalAsync.getLastRequestNumberProcessed(&l_Dummy) \
                          << ", delay 10 seconds and retry.";
            l_MsgSent = true;
            usleep(10000000);   // Delay 10 seconds
        }
        if (l_MsgSent)
        {
            LOG(bb,debug) << "BBLocalAsync::BBPruneMetadata(): >>>>> RESUME <<<<< l_RequestNumber " << l_RequestNumber \
                          << ", g_LocalAsync.getLastRequestNumberProcessed() " << g_LocalAsync.getLastRequestNumberProcessed(&l_Dummy);
        }

        BBPruneMetadataBranch* l_Request = new BBPruneMetadataBranch(l_PathToRemove.string());
        l_RequestNumber = g_LocalAsync.issueAsyncRequest(l_Request);
        l_MsgSent = false;
        while (l_RequestNumber > g_LocalAsync.getLastRequestNumberProcessed(&l_Dummy))
        {
            LOG(bb,debug) << "BBLocalAsync::BBPruneMetadata(): >>>>> DELAY <<<<< l_RequestNumber " << l_RequestNumber \
                          << ", g_LocalAsync.getLastRequestNumberProcessed() " << g_LocalAsync.getLastRequestNumberProcessed(&l_Dummy) \
                          << ", delay 1 second and retry.";
            l_MsgSent = true;
            usleep(1000000);   // Delay 1 second
        }
        if (l_MsgSent)
        {
            LOG(bb,debug) << "BBLocalAsync::BBPruneMetadata(): >>>>> RESUME <<<<< l_RequestNumber " << l_RequestNumber \
                          << ", g_LocalAsync.getLastRequestNumberProcessed() " << g_LocalAsync.getLastRequestNumberProcessed(&l_Dummy);
        }
    }
    catch (ExceptionBailout& e) { }
    catch (std::exception& e)
    {
        LOG_ERROR_WITH_EXCEPTION(__FILE__, __FUNCTION__, __LINE__, e);
        LOG(bb,error) << "BBLocalAsync::BBPruneMetadata(): Unsuccessful removal of cross-bbServer metadata at " << path.c_str() \
                      << ". This cross-bbServer metadata must be manually removed.";
    }

    LOG(bb,info) << "BBLocalAsync::BBPruneMetadata():   END: Removal of cross-bbServer metadata at " << path;

    return;
}

void BBPruneMetadataBranch::doit()
{
    try
    {
        bool l_AllDone = false;
        while (!l_AllDone)
        {
            l_AllDone = true;
            if (!highIB_Activity())
            {
                try
                {
                    bfs::path l_PathToRemove = bfs::path(path);
                    bfs::remove_all(l_PathToRemove);
                    LOG(bb,debug) << "BBLocalAsync::BBPruneMetadataBranch(): Successful removal of cross-bbServer metadata at " << path.c_str();
                }
                catch (std::exception& e2)
                {
                    LOG_ERROR_WITH_EXCEPTION(__FILE__, __FUNCTION__, __LINE__, e2);
                    LOG(bb,error) << "BBLocalAsync::BBPruneMetadataBranch(): Unsuccessful removal of cross-bbServer metadata at " << path.c_str() \
                                  << ". This cross-bbServer metadata must be manually removed.";
                }
            }
            else
            {
                l_AllDone = false;
                LOG(bb,debug) << "BBLocalAsync::BBPruneMetadataBranch(): IB activity too high to do removal at " << path.c_str() \
                              << ". Current " << g_IB_Adapter << " port_rcv_data delta " << g_Last_Port_Rcv_Data_Delta \
                              << ", current " << g_IB_Adapter << " port_xmit_data delta " << g_Last_Port_Xmit_Data_Delta \
                              << ", current IB stats low activity clip value " << g_IBStatsLowActivityClipValue \
                              << ". Delaying " << g_DiskStatsRate << " seconds...";
                sleep(g_DiskStatsRate);
            }
        }
    }
    catch (ExceptionBailout& e) { }
    catch (std::exception& e)
    {
        LOG_ERROR_WITH_EXCEPTION(__FILE__, __FUNCTION__, __LINE__, e);
    }

    return;
}


/*
 * dump() methods
 */
void BBLocalRequest::dump(const char* pPrefix)
{
    stringstream dumpData;
    if (strlen(pPrefix))
    {
        dumpData << pPrefix;
    }
    dumpRequest(dumpData);
    LOG(bb,info) << dumpData.str();

    return;
}

void BBCleanUpContribId::dump(const char* pPrefix)
{
    stringstream dumpData;

    if (strlen(pPrefix))
    {
        dumpData << pPrefix;
    }
    dumpRequest(dumpData);
    dumpData << ", jobid " << tagid.getJobId() \
             << ", jobstepid " << tagid.getJobStepId() \
             << ", tag "<< tagid.getTag() << ", handle "<< handle \
             << ", contribid "<< contribid;
    LOG(bb,info) << dumpData.str();

    return;
}

void BBCleanUpTagInfo::dump(const char* pPrefix)
{
    stringstream dumpData;

    if (strlen(pPrefix))
    {
        dumpData << pPrefix;
    }
    dumpRequest(dumpData);
    dumpData << "Connection " << connection_name << ", " << lvkey \
             << ", jobid " << tagid.getJobId() \
             << ", jobstepid " << tagid.getJobStepId() \
             << ", tag "<< tagid.getTag();
    LOG(bb,info) << dumpData.str();

    return;
}

void BBLogIt::dump(const char* pPrefix)
{
    stringstream dumpData;

    if (strlen(pPrefix))
    {
        dumpData << pPrefix;
    }
    dumpRequest(dumpData);
    dumpData << ", data " << data;
    LOG(bb,info) << dumpData.str();

    return;
}

void BBPruneMetadata::dump(const char* pPrefix)
{
    stringstream dumpData;

    if (strlen(pPrefix))
    {
        dumpData << pPrefix;
    }
    dumpRequest(dumpData);
    dumpData << ", trunk path " << path;
    LOG(bb,info) << dumpData.str();

    return;
}

void BBPruneMetadataBranch::dump(const char* pPrefix)
{
    stringstream dumpData;

    if (strlen(pPrefix))
    {
        dumpData << pPrefix;
    }
    dumpRequest(dumpData);
    dumpData << ", branch path " << path;
    LOG(bb,info) << dumpData.str();

    return;
}
