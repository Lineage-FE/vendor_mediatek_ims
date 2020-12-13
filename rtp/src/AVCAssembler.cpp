/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "[VT][RTP]AVCAssembler"
#include <utils/Log.h>

#include "AVCAssembler.h"

#include "RTPSource.h"
#include <cutils/properties.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/foundation/hexdump.h>

#include <arpa/inet.h>
#include <stdint.h>
#include <inttypes.h>

#define ATRACE_TAG ATRACE_TAG_VIDEO
#include <utils/Trace.h>

namespace imsma
{

// static
AVCAssembler::AVCAssembler(const sp<AMessage> notify)
    : mNotifyMsg(notify->dup()),
      //mAccessUnitRTPTime(0),
      mNextExpectedSeqNoValid(false),
      mNextExpectedSeqNo(0),
      mAccessUnitDamaged(false)
{

    ALOGI("%s",__FUNCTION__);

    mAccuCount = 0;
    mpNALFragmentInfo = NULL;

    mLastLost = -1;
    mLastPacketReceiveTime = 0;

    mLostCount = 0;
    mIDamageNum = 0;

#ifdef DEBUG_DUMP_ACCU
    mDumpAcuu = 0;
    mAccuFd = -1;
    ALOGD("dump downlink accu init=%" PRId64 "", mDumpAcuu);
    char dump_param[PROPERTY_VALUE_MAX];
    memset(dump_param,0,sizeof(dump_param));

    //int64_t dump_value;
    if(property_get("vendor.vt.imsma.dump_downlink_accu", dump_param, NULL)) {
        mDumpAcuu = atol(dump_param);
        ALOGD("dump downlink accu =%" PRId64 "", mDumpAcuu);
    }

    if(mDumpAcuu > 0) {
        const char* accu_filename = "/sdcard/downlink_accu_data.raw";
        mAccuFd = open(accu_filename, O_CREAT | O_LARGEFILE | O_TRUNC | O_RDWR, S_IRUSR | S_IWUSR);
        ALOGD("open %s,accuFd(%d)",accu_filename,mAccuFd);
    }

#endif
}

AVCAssembler::~AVCAssembler()
{
    ALOGI("%s",__FUNCTION__);
#ifdef DEBUG_DUMP_ACCU

    if(mAccuFd >= 0) {
        ALOGD("close accuFd(%d)",mAccuFd);
        close(mAccuFd);
        mAccuFd = -1;
    }

#endif
}

RTPAssembler::AssemblyStatus AVCAssembler::addNALUnit(
    const sp<RTPSource> &source)
{
    ATRACE_CALL();
    ALOGV("%s",__FUNCTION__);
    List<sp<ABuffer> > *queue = source->queue();

    if(queue->empty()) {
        ALOGV("%s,source queue is empty",__FUNCTION__);
        return NOT_ENOUGH_DATA;
    }

    if(mNextExpectedSeqNoValid) {
        List<sp<ABuffer> >::iterator it = queue->begin();

        while(it != queue->end()) {
            if((uint32_t)(*it)->int32Data() >= mNextExpectedSeqNo) {
                break;
            }

            ALOGD("%s,drop unexpected SeqNo(%d) of source queue, mNextExpectedSeqNo(%d)", \
                  __FUNCTION__, (uint32_t)(*it)->int32Data(), mNextExpectedSeqNo);
            it = queue->erase(it);
        }

        if(queue->empty()) {
            return NOT_ENOUGH_DATA;
        }
    }

    sp<ABuffer> buffer = *queue->begin();

    if(!mNextExpectedSeqNoValid) {
        mNextExpectedSeqNoValid = true;
        mNextExpectedSeqNo = (uint32_t) buffer->int32Data();
        ALOGI("%s,first seq = %d",__FUNCTION__,mNextExpectedSeqNo);
    } else if((uint32_t) buffer->int32Data() != mNextExpectedSeqNo) {
        int64_t recv_time = 0;

        if(buffer->meta()->findInt64("recv-time",&recv_time)) {
            uint32_t nowseq = (uint32_t)buffer->int32Data();
            uint32_t nowexpectseq = mNextExpectedSeqNo;

            int64_t diff_time = recv_time - mLastPacketReceiveTime;
            if(diff_time < 0)
            {
                ALOGD("change diff_time for skip all lost judge %lld", (long long)diff_time);
                diff_time = -diff_time;
            }

            if((mLastPacketReceiveTime != 0) && (diff_time > 200000)) {
                ALOGD("skip all lost, NextExpectedSeqNo=%d, NowSeq=%d, time now(%lld)  last(%lld) diff=%lld",
                      mNextExpectedSeqNo, nowseq,
                      (long long) recv_time, (long long) mLastPacketReceiveTime,
                      (long long) diff_time);

                if(nowseq > nowexpectseq) {
                    for(uint32_t i = nowexpectseq; i < nowseq; i++) {
                        packetLostRegister();
                    }
                }

                return SKIP_MISS_PACKET;
            }


            AssemblyStatus assemble_status = getAssembleStatus(queue, mNextExpectedSeqNo);
            ALOGV("%s,%d is not the sequence number %d I expected,status=%d",
                  __FUNCTION__,buffer->int32Data(), mNextExpectedSeqNo,assemble_status);
            return assemble_status;
            //return WRONG_SEQUENCE_NUMBER;
        } else {
            ALOGE("receive data not rece-time!!");
        }
    }

    //record last vaild receive time
    int64_t recv_time = 0;

    if(buffer->meta()->findInt64("recv-time",&recv_time)) {
        mLastPacketReceiveTime = recv_time;
    } else {
        mLastPacketReceiveTime = 0;
        ALOGE("record last vaild time fail");
    }

    //ATRACE_INT64("RTR:AVCAsmb:deqSqN",(int64_t)(uint32_t)(buffer->int32Data()));

    sp<AMessage> buffer_meta = buffer->meta();
    //use seqNum before extending
    int32_t seqNum = 0;
    buffer_meta->findInt32("token",&seqNum);
    ATRACE_ASYNC_END("RTR-MAR:SeqN",seqNum);

    const uint8_t *data = buffer->data();
    size_t size = buffer->size();

    if(size < 1 || (data[0] & 0x80)) {
        // Corrupt.

        ALOGW("Ignoring corrupt buffer.");
        queue->erase(queue->begin());

        ++mNextExpectedSeqNo;
        return MALFORMED_PACKET;
    }

    unsigned nalType = data[0] & 0x1f;

    if(nalType >= 1 && nalType <= 23) {
        ALOGV("%s,single NAL",__FUNCTION__);

        //may be the last one~several FU-A lost before this NAL
        if(mpNALFragmentInfo.get()) {
            //ALOGW("mpNALFragmentInfo has FU-As before this signal NAL");
            if(mAccessUnitDamaged) {
                mpNALFragmentInfo->mIsDamaged = true;
            }

            sp<ABuffer> accu = assembleToNAL(mpNALFragmentInfo);
            submitAccessUnit(accu);

            mAccessUnitDamaged = false;
            mpNALFragmentInfo = NULL;
        }

        ATRACE_ASYNC_BEGIN("RTR-MAR",mAccuCount);
        buffer_meta->setInt32("EarliestPacket_token",seqNum);
        buffer_meta->setInt32("FirstPacket_token",seqNum);
        buffer_meta->setInt32("latestPacekt_token",seqNum);

        addSingleNALUnit(buffer);
        queue->erase(queue->begin());
        ++mNextExpectedSeqNo;
        return OK;
    } else if(nalType == 28) {
        ALOGV("%s,FU-A",__FUNCTION__);
        // FU-A
        return addFragmentedNALUnit(queue);
    } else if(nalType == 24) {
        // STAP-A
        ALOGV("%s,STAP-A",__FUNCTION__);

        //may be the last one~several FU-A lost before this NAL
        if(mpNALFragmentInfo.get()) {
            //ALOGW("mpNALFragmentInfo has FU-As before this STAP-A");
            if(mAccessUnitDamaged) {
                mpNALFragmentInfo->mIsDamaged = true;
            }

            sp<ABuffer> accu = assembleToNAL(mpNALFragmentInfo);
            submitAccessUnit(accu);

            mAccessUnitDamaged = false;
            mpNALFragmentInfo = NULL;
        }

        bool success = addSingleTimeAggregationPacket(buffer);
        queue->erase(queue->begin());
        ++mNextExpectedSeqNo;
        return success ? OK : MALFORMED_PACKET;
    } else if(nalType == 0) {
        ALOGW("%s,Ignoring undefined nal type.",__FUNCTION__);

        queue->erase(queue->begin());
        ++mNextExpectedSeqNo;
        return OK;
    } else {
        ALOGW("%s,Ignoring unsupported buffer (nalType=%d)",__FUNCTION__,nalType);

        queue->erase(queue->begin());
        ++mNextExpectedSeqNo;
        return MALFORMED_PACKET;
    }
}

void AVCAssembler::addSingleNALUnit(const sp<ABuffer> &buffer)
{
    //ALOGD("addSingleNALUnit of size %zu", buffer->size());
    submitAccessUnit(buffer);
    return;
}

bool AVCAssembler::addSingleTimeAggregationPacket(const sp<ABuffer> &buffer)
{

    const uint8_t *data = buffer->data();
    size_t size = buffer->size();
    //ALOGD("%s,buffer size(%d)",__FUNCTION__,size);

    if(size < 3) {
        ALOGV("Discarding too small STAP-A packet.");
        return false;
    }

    sp<AMessage> meta = buffer->meta();
    int32_t token = 0;
    meta->findInt32("token",&token);

    ++data;
    --size;

    while(size >= 2) {
        size_t nalSize = (data[0] << 8) | data[1];

        if(size < nalSize + 2) {
            ALOGV("Discarding malformed STAP-A packet.");
            return false;
        }

        ATRACE_ASYNC_BEGIN("RTR-MAR",mAccuCount);

        sp<ABuffer> unit = new ABuffer(nalSize);
        memcpy(unit->data(), &data[2], nalSize);

        CopyMetas(unit,buffer);

        data += 2 + nalSize;
        size -= 2 + nalSize;

        if(size > 2) {
            //this is not the last nal of STAP
            //re-clear the marker bit flag
            sp<AMessage> unit_meta = unit->meta();
            unit_meta->setInt32("M",0);
        }

        unit->meta()->setInt32("EarliestPacket_token",token);
        unit->meta()->setInt32("FirstPacket_token",token);
        unit->meta()->setInt32("latestPacekt_token",token);

        addSingleNALUnit(unit);
    }

    if(size != 0) {
        ALOGV("Unexpected padding at end of STAP-A packet.");
    }

    return true;
}

RTPAssembler::AssemblyStatus AVCAssembler::addFragmentedNALUnit(
    List<sp<ABuffer> > *queue)
{
    ATRACE_CALL();
    CHECK(!queue->empty());

    sp<ABuffer> buffer = *queue->begin();
    const uint8_t *data = buffer->data();
    size_t size = buffer->size();
    ALOGV("%s,buffer size(%zu)",__FUNCTION__,size);

    CHECK(size > 0);
    unsigned indicator = data[0];

    CHECK((indicator & 0x1f) == 28);

    if(size < 2) {
        ALOGW("Ignoring malformed FU buffer (size = %zu)", size);

        queue->erase(queue->begin());
        ++mNextExpectedSeqNo;
        return MALFORMED_PACKET;
    }

    uint32_t nalType = data[1] & 0x1f;
    //uint32_t nri = (data[0] >> 5) & 3;

    if(!mpNALFragmentInfo.get()) {
        if(!(data[1] & 0x80)) {
            //ALOGW("Start bit not set on first buffer");

            queue->erase(queue->begin());
            ++mNextExpectedSeqNo;
            return MALFORMED_PACKET;
        } else {
            //ALOGD("This is a new start fragment");
            //need re-set mAccessUnitDamaged for new NAL
            //avoid seting "damage" for new  completed NAL by mistake because of the last whole NAL has lost

            mAccessUnitDamaged = false;
        }
    }

    if(mpNALFragmentInfo.get()) {
        if(data[1] & 0x80) {
            //ALOGD("This is a new start fragment,submit the last nal");
            //may be the end nal fragment of last NAL is lost
            //may be the end flag not set for the end fragement of the last NAL
            if(mAccessUnitDamaged) {
                mpNALFragmentInfo->mIsDamaged = true;

                if(nalType == 5) {
                    mIDamageNum++;
                }
            }

            //maybe the end of NAL fragment is not set for the last NAL
            //submitAccu();
            sp<ABuffer> accu = assembleToNAL(mpNALFragmentInfo);
            submitAccessUnit(accu);
            //because receive a start fragment, re-set mAccessUnitDamaged
            mAccessUnitDamaged = false;
            mpNALFragmentInfo = NULL;
        } else {
            //ALOGD("append fragment to NAL");
            if(nalType != mpNALFragmentInfo->mNALType) {
                ALOGE("Ignoring malformed FU buffer(fragment nal_type(%d) != %d)",\
                      nalType,mpNALFragmentInfo->mNALType);
                queue->erase(queue->begin());
                ++mNextExpectedSeqNo;
                return MALFORMED_PACKET;
            }
        }
    }

    if(!mpNALFragmentInfo.get()) {
        //ALOGD("new one NALFragMentsInfo");
        mpNALFragmentInfo = new NALFragMentsInfo();
        mpNALFragmentInfo->mNALType = nalType;
        //mpNALFragmentInfo->mNRI = nri;
    }

    mpNALFragmentInfo->mNALFragments.push_back(buffer);
    mpNALFragmentInfo->mNALSize += size - 2;
    mpNALFragmentInfo->mTotalCount++;
    ALOGV("%s,Nal-FU-A(count:%d,total_size(%d))",\
          __FUNCTION__,mpNALFragmentInfo->mTotalCount,mpNALFragmentInfo->mNALSize);

    if(data[1] & 0x40) {
        //ALOGD("This is end fragment.");

        if(mAccessUnitDamaged) {
            mpNALFragmentInfo->mIsDamaged = true;

            if(nalType == 5) {
                mIDamageNum++;
            }
        }

        //maybe the end of NAL fragment is not set for the last NAL
        //submitAccu();
        sp<ABuffer> accu = assembleToNAL(mpNALFragmentInfo);
        submitAccessUnit(accu);
        mAccessUnitDamaged = false;
        mpNALFragmentInfo = NULL;
    }

    queue->erase(queue->begin());
    ++mNextExpectedSeqNo;
    //ALOGD("%s,mNextExpectedSeqNo(%d)",__FUNCTION__,mNextExpectedSeqNo);

    return OK;
}
sp<ABuffer> AVCAssembler::assembleToNAL(sp<NALFragMentsInfo> nalFragmentInfo)
{
    uint32_t nal_size = nalFragmentInfo->mNALSize + 1;// for 1 byte NAL header


    List<sp<ABuffer> > *queue = & (nalFragmentInfo->mNALFragments);   //ToDo: can we assigne list this way
    //ALOGI("%s,nal fragments in list(num = %d, total_size=%d)",__FUNCTION__,queue->size(),nal_size);

    ATRACE_ASYNC_BEGIN("RTR-MAR",mAccuCount);

    sp<ABuffer> unit = new ABuffer(nal_size);

    if(nalFragmentInfo->mIsDamaged) {
        unit->meta()->setInt32("damaged", true);
    }

    sp<ABuffer> beginFragment = *queue->begin();
    sp<ABuffer> endFragment = *--queue->end();
    uint8_t* data = beginFragment->data();

    uint32_t nalType = data[1] & 0x1f;
    uint32_t nri = (data[0] >> 5) & 3;
    unit->meta()->setInt32("importance", nri);

    CopyMetas(unit,endFragment);

    unit->data() [0] = (nri << 5) | nalType;

    size_t offset = 1;
    List<sp<ABuffer> >::iterator it = queue->begin();
    sp<AMessage> meta = (*it)->meta();
    int64_t earliest_recv_time = 0;
    meta->findInt64("recv-time",&earliest_recv_time);

    int32_t tokenF = 0;
    (*it)->meta()->findInt32("token",&tokenF);
    unit->meta()->setInt32("FirstPacket_token",tokenF);

    while(it!= queue->end()) {
        const sp<ABuffer> &buffer = *it;

        sp<AMessage> meta = buffer->meta();
        int64_t recv_time = 0;
        meta->findInt64("recv-time",&recv_time);

        int32_t token = 0;
        meta->findInt32("token",&token);

        if(recv_time <= earliest_recv_time) {
            unit->meta()->setInt32("EarliestPacket_token",token);
            earliest_recv_time = recv_time;
        }

        memcpy(unit->data() + offset, buffer->data() + 2, buffer->size() - 2);
        offset += buffer->size() - 2;

        it = queue->erase(it);

        if(it == queue->end()) {
            unit->meta()->setInt32("latestPacekt_token",token);
            ALOGV("F=%d L=%d", tokenF, token);
        }
    }

    unit->setRange(0, nal_size);    //ToDo: assign uint32_t to size_t,ok?
    //ALOGD("successfully assembled a NAL unit(size:%d) from fragments.",nal_size);
    return unit;
}
void AVCAssembler::submitAccessUnit(const sp<ABuffer>& accessUnit)
{
    /*
    if (mAccessUnitDamaged) {
        accessUnit->meta()->setInt32("damaged", true);
    }

    mAccessUnitDamaged = false;
    */
    ATRACE_CALL();
    ALOGV("%s,notify(0x%x)",__FUNCTION__,mNotifyMsg->what());
    sp<AMessage> msg = mNotifyMsg->dup();
    msg->setInt32("what",kWhatAccessUnit);
    msg->setBuffer("access-unit", accessUnit);

    sp<AMessage> accu_meta = accessUnit->meta();
    int32_t marker_bit = 0;
    accu_meta->findInt32("M", &marker_bit);

    if(marker_bit > 0) {
        //ALOGD("%s,last accu of frame",__FUNCTION__);
    }

    accu_meta->setInt32("token",mAccuCount);
    ALOGV("%s,accu count(%d)",__FUNCTION__,mAccuCount);

    int32_t iEarliestPak = 0;
    int32_t iLatestPak = 0;
    accu_meta->findInt32("EarliestPacket_token",&iEarliestPak);
    accu_meta->findInt32("latestPacekt_token",&iLatestPak);
    ATRACE_INT("RTR-MAR:EpkSeqN",iEarliestPak);
    ATRACE_INT("RTR-MAR:LpkSeqN",iLatestPak);

    mAccuCount++;

    //ATRACE_ASYNC_BEGIN("RTR-MAR",mAccuCount);
    //ATRACE_INT("RTR:AVCAsmb:sbAU",mAccuCount);
#ifdef DEBUG_DUMP_ACCU

    if(mAccuFd >= 0) {
        size_t real_write = write(mAccuFd,"\x00\x00\x00\x01",4);
        real_write = write(mAccuFd,accessUnit->data(),accessUnit->size());
        //ALOGD("write to file,real_write(%d)",real_write);
    }

#endif
    msg->post();
}

RTPAssembler::AssemblyStatus AVCAssembler::assembleMore(
    const sp<RTPSource> source)
{
    AssemblyStatus status = addNALUnit(source);

    if(status == MALFORMED_PACKET) {
        mAccessUnitDamaged = true;
    }

    return status;
}

void AVCAssembler::packetLost()
{
    CHECK(mNextExpectedSeqNoValid);

    if(mLastLost <= 0) {
        mLastLost = ALooper::GetNowUs();
    } else {
        if(ALooper::GetNowUs() - mLastLost < 200000ll) {
            //ALOGW("packetLost  don't submit");
            return ;
        }
    }

    //don't submit~~~
    if(mPackLostList.size() == 0) {
        //ALOGW("not packetLost ");
        return ;
    }

    int LostCount = mPackLostList.size();
    ALOGW("packetLost  size=%d", LostCount);

    sp<ABuffer> TmBuf = new ABuffer(4*LostCount);

    uint8_t * pTmBuf = TmBuf->base();

    int count = 0;

    for(List<uint32_t>::iterator i = mPackLostList.begin(); i != mPackLostList.end(); i++) {
        //ALOGW("packetLost  count=%d  seq=%d", count, *i);
        ((uint32_t *) pTmBuf) [count++] = *i;
    }

    sp<AMessage> msg = mNotifyMsg->dup();
    msg->setInt32("what",kWhatPacketLost);
    msg->setInt32("lostcount",LostCount);
    msg->setBuffer("lostpointer",TmBuf);
    msg->post();

    mLastLost = ALooper::GetNowUs();

    mPackLostList.clear();
}

void AVCAssembler::packetLostRegister()
{
    //ALOGW("packetLostRegister (expected %d)", mNextExpectedSeqNo);

    if(mPackLostList.size() >= PACKETLOSTRECORDNUM) {
        List<uint32_t>::iterator i = mPackLostList.begin();
        mPackLostList.erase(i);
    }

    mPackLostList.push_back(mNextExpectedSeqNo);

    ++mNextExpectedSeqNo;
    mAccessUnitDamaged = true;

    mLostCount++;
}
void AVCAssembler::flushQueue()
{
    //because RTPSource will lock the operation with packetReceive
    //so Assembler need not lock
    if(mpNALFragmentInfo.get()) {
        mpNALFragmentInfo->mNALFragments.clear();
        mpNALFragmentInfo = NULL;
    }
}
void AVCAssembler::reset()
{
    //flushQueue();
    mNextExpectedSeqNoValid = false;
    mNextExpectedSeqNo = 0;
    mAccessUnitDamaged = false;
    mLostCount = 0;
    mIDamageNum = 0;
    mLastPacketReceiveTime = 0;

    //maybe we should add lock,but now nobody use the interface
    //mPackLostList.clear();
    mLastLost = -1;
}

bool AVCAssembler::isCSD(const sp<ABuffer>& accessUnit)
{
    const uint8_t *data = accessUnit->data();

    uint32_t nalType = data[0] & 0x1f;

    ALOGI("AVCAssembler isCSD data[0]=0x%x data[1]=0x%x nalType=%d", data[0], data[1], nalType);

    return (nalType == 7 || nalType == 8) ? true : false;
}


}  // namespace android
