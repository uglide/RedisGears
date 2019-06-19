#include "consensus.h"
#include "redisgears_memory.h"
#include "redismodule.h"
#include "utils/dict.h"
#include "utils/buffer.h"
#include "cluster.h"
#include <stdbool.h>
#include <assert.h>

Gears_dict* consensusDict;

static ConsensusInstance* Consensus_InstanceCreate(Consensus* consensus, long long consensusId){
    ConsensusInstance* consensusInstance = RG_CALLOC(1, sizeof(*consensusInstance));
    if(consensusId >= 0){
        consensusInstance->consensusId = consensusId;
        if(consensusId >= consensus->currConsensusId){
            consensus->currConsensusId = consensusId + 1;
        }
    }else{
        consensusInstance->consensusId = consensus->currConsensusId++;
    }

    consensusInstance->phase = PHASE_ONE;

    if(Gears_listLength(consensus->consensusInstances) == 0){
        Gears_listAddNodeHead(consensus->consensusInstances, consensusInstance);
    }else{
        Gears_listNode* node = Gears_listFirst(consensus->consensusInstances);
        do{
            ConsensusInstance* inst = Gears_listNodeValue(node);
            assert(inst->consensusId != consensusInstance->consensusId);
            if(inst->consensusId < consensusInstance->consensusId){
                Gears_listInsertNode(consensus->consensusInstances, node, consensusInstance, 0);
                break;
            }
        }while((node = Gears_listNextNode(node)));
        Gears_listAddNodeTail(consensus->consensusInstances, consensusInstance);
    }

    return consensusInstance;
}

static ConsensusInstance* Consensus_InstanceGetOrCreate(Consensus* consensus, long long consensusId){
    Gears_listIter *iter = Gears_listGetIterator(consensus->consensusInstances, AL_START_HEAD);
    Gears_listNode *node = NULL;
    while((node = Gears_listNext(iter))){
        ConsensusInstance* instance = Gears_listNodeValue(node);
        if(instance->consensusId == consensusId){
            Gears_listReleaseIterator(iter);
            return instance;
        }
        if(instance->consensusId < consensusId){
            break;
        }
    }

    // not found let create one
    Gears_listReleaseIterator(iter);

    ConsensusInstance* instance = Consensus_InstanceCreate(consensus, consensusId);

    return instance;
}

static ConsensusInstance* Consensus_InstanceGet(Consensus* consensus, long long consensusId){
    Gears_listIter *iter = Gears_listGetIterator(consensus->consensusInstances, AL_START_HEAD);
    Gears_listNode *node = NULL;
    while((node = Gears_listNext(iter))){
        ConsensusInstance* instance = Gears_listNodeValue(node);
        if(instance->consensusId == consensusId){
            Gears_listReleaseIterator(iter);
            return instance;
        }
        if(instance->consensusId < consensusId){
            break;
        }
    }

    assert(false);

    return NULL;
}

static void Consensus_LearnValueMessage(RedisModuleCtx *ctx, const char *sender_id, uint8_t type, const unsigned char *payload, uint32_t len){
    Gears_Buffer buff;
    buff.buff = (char*)payload;
    buff.size = len;
    buff.cap = len;
    Gears_BufferReader br;
    Gears_BufferReaderInit(&br, &buff);
    const char* name = Gears_BufferReaderReadString(&br);
    long long consnsusId = Gears_BufferReaderReadLong(&br);
    long long proposalId = Gears_BufferReaderReadLong(&br);
    size_t valLen = 0;
    const char* val = Gears_BufferReaderReadBuff(&br, &valLen);

    Consensus* consensus = Gears_dictFetchValue(consensusDict, name);

    ConsensusInstance* instance = Consensus_InstanceGet(consensus, consnsusId);

    if(instance->learner.proposalId > proposalId){
        return;
    }

    if(instance->learner.proposalId < proposalId){
        instance->learner.proposalId = proposalId;
        instance->learner.learnedNumber = 1;
        return;
    }

    ++instance->learner.learnedNumber;

    if(instance->learner.learnedNumber == (Cluster_GetSize() / 2) + 1){
        if(!instance->learner.valueLeared){
            bool isSameValue = true;
            if(instance->learner.originalVal){
                if(valLen != instance->learner.originalLen){
                    isSameValue = false;
                }else{
                    if(memcmp(val, instance->learner.originalVal, valLen) != 0){
                        isSameValue = false;
                    }
                }
            }

            if(isSameValue){
                consensus->approvedCallback(consensus->privateData, val, len, instance->additionalData);
            }else{
                consensus->approvedCallback(consensus->privateData, val, len, NULL);
                Consensus_Send(consensus, instance->learner.originalVal, instance->learner.originalLen, instance->additionalData);
            }

            instance->learner.valueLeared = true;
        }
    }
}

static void Consensus_ValueAcceptedMessage(RedisModuleCtx *ctx, const char *sender_id, uint8_t type, const unsigned char *payload, uint32_t len){
    Gears_Buffer buff;
    buff.buff = (char*)payload;
    buff.size = len;
    buff.cap = len;
    Gears_BufferReader br;
    Gears_BufferReaderInit(&br, &buff);
    const char* name = Gears_BufferReaderReadString(&br);
    long long consnsusId = Gears_BufferReaderReadLong(&br);
    long long proposalId = Gears_BufferReaderReadLong(&br);

    Consensus* consensus = Gears_dictFetchValue(consensusDict, name);

    ConsensusInstance* instance = Consensus_InstanceGet(consensus, consnsusId);

    if(instance->phase != PHASE_TWO){
        // not in phase 2, just ignore it!!
        return;
    }

    if(instance->proposer.proposalId != proposalId){
        assert(instance->proposer.proposalId > proposalId);
        // this is an old reply, just ignore it!!
        return;
    }

    instance->proposer.acceptedNumber++;

    if(instance->proposer.acceptedNumber == (Cluster_GetSize() / 2) + 1){
        // we are done, we can rest now.
        instance->phase = PHASE_DONE;
    }
}

static void Consensus_AcceptDeniedMessage(RedisModuleCtx *ctx, const char *sender_id, uint8_t type, const unsigned char *payload, uint32_t len){
    Gears_Buffer buff;
    buff.buff = (char*)payload;
    buff.size = len;
    buff.cap = len;
    Gears_BufferReader br;
    Gears_BufferReaderInit(&br, &buff);
    const char* name = Gears_BufferReaderReadString(&br);
    long long consnsusId = Gears_BufferReaderReadLong(&br);
    long long proposalId = Gears_BufferReaderReadLong(&br);

    Consensus* consensus = Gears_dictFetchValue(consensusDict, name);

    ConsensusInstance* instance = Consensus_InstanceGet(consensus, consnsusId);

    if(instance->phase != PHASE_TWO){
        // not in phase 2, just ignore it!!
        return;
    }

    if(instance->proposer.proposalId > proposalId){
        // this is an old deny reply, we can ignore it.
        return;
    }

    instance->proposer.proposalId = proposalId + 1;
    instance->proposer.acceptedNumber = 0;
    instance->proposer.recruitedNumber = 0;
    instance->proposer.biggerProposalId = 0;
    instance->phase = PHASE_ONE;

    Gears_Buffer *buf = Gears_BufferCreate();
    Gears_BufferWriter bw;
    Gears_BufferWriterInit(&bw, buf);
    Gears_BufferWriterWriteString(&bw, consensus->name);
    Gears_BufferWriterWriteLong(&bw, instance->consensusId);
    Gears_BufferWriterWriteLong(&bw, instance->proposer.proposalId);

    Cluster_SendMsgToAllAndMyselfM(Consensus_RecruitMessage, buf->buff, buf->size);

    Gears_BufferFree(buf);
}

static void Consensus_AcceptMessage(RedisModuleCtx *ctx, const char *sender_id, uint8_t type, const unsigned char *payload, uint32_t len){
    Gears_Buffer buff;
    buff.buff = (char*)payload;
    buff.size = len;
    buff.cap = len;
    Gears_BufferReader br;
    Gears_BufferReaderInit(&br, &buff);
    const char* name = Gears_BufferReaderReadString(&br);
    long long consnsusId = Gears_BufferReaderReadLong(&br);
    long long proposalId = Gears_BufferReaderReadLong(&br);
    size_t valLen = 0;
    const char* val = Gears_BufferReaderReadBuff(&br, &valLen);

    Consensus* consensus = Gears_dictFetchValue(consensusDict, name);

    ConsensusInstance* instance = Consensus_InstanceGet(consensus, consnsusId);

    if(instance->acceptor.proposalId != proposalId){
        assert(instance->acceptor.proposalId > proposalId);
        Gears_Buffer *buf = Gears_BufferCreate();
        Gears_BufferWriter bw;
        Gears_BufferWriterInit(&bw, buf);
        Gears_BufferWriterWriteString(&bw, consensus->name);
        Gears_BufferWriterWriteLong(&bw, instance->consensusId);
        Gears_BufferWriterWriteLong(&bw, instance->proposer.proposalId);
        Cluster_SendMsgM(sender_id, Consensus_AcceptDeniedMessage, buf->buff, buf->size);
        Gears_BufferFree(buf);
        return;
    }

    // accepting the value
    instance->acceptor.val = RG_ALLOC(valLen);
    memcpy(instance->acceptor.val, val, valLen);
    instance->acceptor.len = valLen;

    Gears_Buffer *buf = Gears_BufferCreate();
    Gears_BufferWriter bw;
    Gears_BufferWriterInit(&bw, buf);
    Gears_BufferWriterWriteString(&bw, consensus->name);
    Gears_BufferWriterWriteLong(&bw, instance->consensusId);
    Gears_BufferWriterWriteLong(&bw, instance->acceptor.proposalId);
    Cluster_SendMsgM(sender_id, Consensus_ValueAcceptedMessage, buf->buff, buf->size);
    Gears_BufferWriterWriteBuff(&bw, instance->acceptor.val, instance->acceptor.len);
    Cluster_SendMsgToAllAndMyselfM(Consensus_LearnValueMessage, buf->buff, buf->size);

    Gears_BufferFree(buf);
}

static void Consensus_RecruitedMessage(RedisModuleCtx *ctx, const char *sender_id, uint8_t type, const unsigned char *payload, uint32_t len){
    Gears_Buffer buff;
    buff.buff = (char*)payload;
    buff.size = len;
    buff.cap = len;
    Gears_BufferReader br;
    Gears_BufferReaderInit(&br, &buff);
    const char* name = Gears_BufferReaderReadString(&br);
    long long consnsusId = Gears_BufferReaderReadLong(&br);
    long long proposalId = Gears_BufferReaderReadLong(&br);
    long long oldProposalId = Gears_BufferReaderReadLong(&br);
    long long hasValue = Gears_BufferReaderReadLong(&br);
    const char* val = NULL;
    size_t valLen = 0;
    if(hasValue){
        val = Gears_BufferReaderReadBuff(&br, &valLen);
    }

    Consensus* consensus = Gears_dictFetchValue(consensusDict, name);

    ConsensusInstance* instance = Consensus_InstanceGet(consensus, consnsusId);

    if(instance->phase != PHASE_ONE){
        // we are not in phase one anymore, we can ignore this deny message
        return;
    }

    if(instance->proposer.proposalId != proposalId){
        assert(instance->proposer.proposalId > proposalId);
        // this is an old reply, just ignore it!!
        return;
    }

    if(hasValue && instance->proposer.biggerProposalId < oldProposalId){
        bool switchVals = false;
        if(valLen != instance->proposer.len){
            switchVals = true;
        }else{
            if(memcmp(instance->proposer.val, val, valLen) != 0){
                switchVals = true;
            }
        }

        if(switchVals){
            RG_FREE(instance->proposer.val);
            instance->proposer.val = RG_ALLOC(valLen);
            memcpy(instance->proposer.val, val, valLen);
            instance->proposer.len = valLen;
        }
        instance->proposer.biggerProposalId = oldProposalId;
    }

    instance->proposer.recruitedNumber++;

    if(instance->proposer.recruitedNumber == (Cluster_GetSize() / 2) + 1){
        assert(sender_id); // not possible that we are the last one to accept ourself
        Gears_Buffer *buf = Gears_BufferCreate();
        Gears_BufferWriter bw;
        Gears_BufferWriterInit(&bw, buf);
        Gears_BufferWriterWriteString(&bw, consensus->name);
        Gears_BufferWriterWriteLong(&bw, instance->consensusId);
        Gears_BufferWriterWriteLong(&bw, instance->proposer.proposalId);
        Gears_BufferWriterWriteBuff(&bw, instance->proposer.val, instance->proposer.len);
        Cluster_SendMsgToAllAndMyselfM(Consensus_AcceptMessage, buf->buff, buf->size);

        Gears_BufferFree(buf);

        instance->phase = PHASE_TWO;
    }
}

static void Consensus_DeniedMessage(RedisModuleCtx *ctx, const char *sender_id, uint8_t type, const unsigned char *payload, uint32_t len){
    // one deny tells us to restart
    Gears_Buffer buff;
    buff.buff = (char*)payload;
    buff.size = len;
    buff.cap = len;
    Gears_BufferReader br;
    Gears_BufferReaderInit(&br, &buff);
    const char* name = Gears_BufferReaderReadString(&br);
    long long consnsusId = Gears_BufferReaderReadLong(&br);
    long long proposalId = Gears_BufferReaderReadLong(&br);

    Consensus* consensus = Gears_dictFetchValue(consensusDict, name);

    ConsensusInstance* instance = Consensus_InstanceGet(consensus, consnsusId);

    if(instance->phase != PHASE_ONE){
        // we are not in phase one anymore, we can ignore this deny message
        return;
    }

    if(instance->proposer.proposalId > proposalId){
        // this is an old deny reply, we can ignore it.
        return;
    }

    instance->proposer.proposalId = proposalId + 1;
    instance->proposer.acceptedNumber = 0;
    instance->proposer.recruitedNumber = 0;
    instance->proposer.biggerProposalId = 0;

    Gears_Buffer *buf = Gears_BufferCreate();
    Gears_BufferWriter bw;
    Gears_BufferWriterInit(&bw, buf);
    Gears_BufferWriterWriteString(&bw, consensus->name);
    Gears_BufferWriterWriteLong(&bw, instance->consensusId);
    Gears_BufferWriterWriteLong(&bw, instance->proposer.proposalId);

    Cluster_SendMsgToAllAndMyselfM(Consensus_RecruitMessage, buf->buff, buf->size);

    Gears_BufferFree(buf);
}

static void Consensus_RecruitMessage(RedisModuleCtx *ctx, const char *sender_id, uint8_t type, const unsigned char *payload, uint32_t len){
    Gears_Buffer buff;
    buff.buff = (char*)payload;
    buff.size = len;
    buff.cap = len;
    Gears_BufferReader br;
    Gears_BufferReaderInit(&br, &buff);
    const char* name = Gears_BufferReaderReadString(&br);
    long long consnsusId = Gears_BufferReaderReadLong(&br);
    long long proposalId = Gears_BufferReaderReadLong(&br);

    Consensus* consensus = Gears_dictFetchValue(consensusDict, name);

    ConsensusInstance* instance = Consensus_InstanceGetOrCreate(consensus, consnsusId);

    bool recruited = false;
    long long oldPorposalId = instance->acceptor.proposalId;

    if(proposalId > instance->acceptor.proposalId){
        // Recruited, adobt the proposal id
        instance->acceptor.proposalId = proposalId;
        recruited = true;
    }
    Gears_Buffer *reply = Gears_BufferCreate();
    Gears_BufferWriter bw;
    Gears_BufferWriterInit(&bw, reply);
    Gears_BufferWriterWriteString(&bw, consensus->name);
    Gears_BufferWriterWriteLong(&bw, instance->consensusId);
    Gears_BufferWriterWriteLong(&bw, instance->acceptor.proposalId);
    Gears_BufferWriterWriteLong(&bw, oldPorposalId);

    if(recruited){
        if(instance->acceptor.val){
            Gears_BufferWriterWriteLong(&bw, 1); // has value
            Gears_BufferWriterWriteBuff(&bw, instance->acceptor.val, instance->acceptor.len);
        }else{
            Gears_BufferWriterWriteLong(&bw, 0); // no value
        }
        Cluster_SendMsgM(sender_id, Consensus_RecruitedMessage, reply->buff, reply->size);
    }else{
        Cluster_SendMsgM(sender_id, Consensus_DeniedMessage, reply->buff, reply->size);
    }

    Gears_BufferFree(reply);
}

typedef struct ConsensusMsgCtx{
    Consensus* consensus;
    char* msg;
    size_t len;
    void* additionalData;
}ConsensusMsgCtx;

static void Consensus_StartInstance(RedisModuleCtx *ctx, const char *sender_id, uint8_t type, const unsigned char *payload, uint32_t len){
    ConsensusMsgCtx* cmctx = (*(ConsensusMsgCtx**)payload);

    ConsensusInstance* consensusInstance = Consensus_InstanceCreate(cmctx->consensus, -1);

    consensusInstance->proposer.val = (char*)cmctx->msg;
    consensusInstance->proposer.len = cmctx->len;
    consensusInstance->additionalData = cmctx->additionalData;

    consensusInstance->learner.originalVal = RG_ALLOC(cmctx->len);
    memcpy(consensusInstance->learner.originalVal, cmctx->msg, cmctx->len);
    consensusInstance->learner.originalLen = cmctx->len;

    Gears_Buffer *buff = Gears_BufferCreate();
    Gears_BufferWriter bw;
    Gears_BufferWriterInit(&bw, buff);
    Gears_BufferWriterWriteString(&bw, cmctx->consensus->name);
    Gears_BufferWriterWriteLong(&bw, consensusInstance->consensusId);
    Gears_BufferWriterWriteLong(&bw, consensusInstance->proposer.proposalId);

    Cluster_SendMsgToAllAndMyselfM(Consensus_RecruitMessage, buff->buff, buff->size);

    RG_FREE(cmctx);

    Gears_BufferFree(buff);
}

static void Consensus_TestOnMsgAproved(void* privateData, const char* msg, size_t len, void* additionalData){
    printf("message arrived : %s\r\n", msg);
}

static int Consensus_Test(RedisModuleCtx *ctx, RedisModuleString **argv, int argc){
    if(argc != 2){
        return RedisModule_WrongArity(ctx);
    }

    if(!Cluster_IsClusterMode()){
        RedisModule_ReplyWithError(ctx, "can not test consensus on none cluster mode");
        return REDISMODULE_OK;
    }

    size_t len;
    const char* msg = RedisModule_StringPtrLen(argv[1], &len);

    Consensus* consensus = Gears_dictFetchValue(consensusDict, "TestConsensus");

    Consensus_Send(consensus, msg, len, NULL);

    RedisModule_ReplyWithSimpleString(ctx, "OK");
    return REDISMODULE_OK;
}

int Consensus_Init(RedisModuleCtx* ctx){
    consensusDict = Gears_dictCreate(&Gears_dictTypeHeapStrings, NULL);
    Cluster_RegisterMsgReceiverM(Consensus_StartInstance);
    Cluster_RegisterMsgReceiverM(Consensus_RecruitMessage);
    Cluster_RegisterMsgReceiverM(Consensus_RecruitedMessage);
    Cluster_RegisterMsgReceiverM(Consensus_DeniedMessage);
    Cluster_RegisterMsgReceiverM(Consensus_AcceptMessage);
    Cluster_RegisterMsgReceiverM(Consensus_AcceptDeniedMessage);
    Cluster_RegisterMsgReceiverM(Consensus_ValueAcceptedMessage);
    Cluster_RegisterMsgReceiverM(Consensus_LearnValueMessage);

    Consensus_Create("TestConsensus", Consensus_TestOnMsgAproved, NULL);

    if (RedisModule_CreateCommand(ctx, "rg.testconsensus", Consensus_Test, "readonly", 0, 0, 0) != REDISMODULE_OK){
        RedisModule_Log(ctx, "warning", "could not register command rg.testconsensus");
        return REDISMODULE_ERR;
    }

    return REDISMODULE_OK;
}

Consensus* Consensus_Create(const char* name, Consensus_OnMsgAproved approvedCallback, void* privateData){
    Consensus* consensus = RG_ALLOC(sizeof(*consensus));
    consensus->name = RG_STRDUP(name);
    consensus->approvedCallback = approvedCallback;
    consensus->privateData = privateData;
    consensus->currConsensusId = 0;
    consensus->consensusInstances = Gears_listCreate();

    Gears_dictAdd(consensusDict, consensus->name, consensus);

    return consensus;
}

void Consensus_Send(Consensus* consensus, const char* msg, size_t len, void* additionalData){
    ConsensusMsgCtx* cmctx = RG_ALLOC(sizeof(ConsensusMsgCtx));
    cmctx->consensus = consensus;
    cmctx->msg = RG_ALLOC(len);
    memcpy(cmctx->msg, msg, len);
    cmctx->len = len;
    cmctx->additionalData = additionalData;
    Cluster_SendMsgToMySelfM(Consensus_StartInstance, (char*)&cmctx, sizeof(ConsensusMsgCtx*));
}




