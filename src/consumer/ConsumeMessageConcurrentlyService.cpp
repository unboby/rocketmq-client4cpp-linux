/**
* Copyright (C) 2013 kangliqiang ,kangliq@163.com
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/
#include "ConsumeMessageConcurrentlyService.h"

#include "DefaultMQPushConsumerImpl.h"
#include "MessageListener.h"
#include "MessageQueue.h"
#include "RebalanceImpl.h"
#include "DefaultMQPushConsumer.h"
#include "MixAll.h"
#include "KPRUtil.h"
#include "OffsetStore.h"

namespace rmq
{

ConsumeMessageConcurrentlyService::ConsumeMessageConcurrentlyService(
    DefaultMQPushConsumerImpl* pDefaultMQPushConsumerImpl,
    MessageListenerConcurrently* pMessageListener)
{
    m_pDefaultMQPushConsumerImpl = pDefaultMQPushConsumerImpl;
    m_pMessageListener = pMessageListener;
    m_pDefaultMQPushConsumer = m_pDefaultMQPushConsumerImpl->getDefaultMQPushConsumer();
    m_consumerGroup = m_pDefaultMQPushConsumer->getConsumerGroup();
    m_pConsumeExecutor = new kpr::ThreadPool("ConsumeMessageThreadPool", 5,
    	m_pDefaultMQPushConsumer->getConsumeThreadMin(), m_pDefaultMQPushConsumer->getConsumeThreadMax());
    m_scheduledExecutorService = new kpr::TimerThread("ConsumeMessageConcurrentlyService", 1000);
}

ConsumeMessageConcurrentlyService::~ConsumeMessageConcurrentlyService()
{
}


void ConsumeMessageConcurrentlyService::start()
{
    m_scheduledExecutorService->Start();
}

void ConsumeMessageConcurrentlyService::shutdown()
{
    m_pConsumeExecutor->Destroy();
    m_scheduledExecutorService->Stop();
    m_scheduledExecutorService->Join();
}

ConsumerStat& ConsumeMessageConcurrentlyService::getConsumerStat()
{
    return m_pDefaultMQPushConsumerImpl->getConsumerStatManager()->getConsumertat();
}

bool ConsumeMessageConcurrentlyService::sendMessageBack(MessageExt& msg,
        ConsumeConcurrentlyContext& context)
{
    // ����û�û�����ã���������������Դ����Զ�������ʱʱ��
    try
    {
        m_pDefaultMQPushConsumerImpl->sendMessageBack(msg,
        	context.delayLevelWhenNextConsume, context.messageQueue.getBrokerName());
        return true;
    }
    catch (...)
    {
		RMQ_ERROR("sendMessageBack exception, group: %s, msg: %s",
			m_consumerGroup.c_str(), msg.toString().c_str());
    }

    return false;
}

class SubmitConsumeRequestLater : public kpr::TimerHandler
{
public:
    SubmitConsumeRequestLater(std::list<MessageExt*>& msgs,
                              ProcessQueue* pProcessQueue,
                              MessageQueue messageQueue,
                              ConsumeMessageConcurrentlyService* pService)
        : m_msgs(msgs),
          m_pProcessQueue(pProcessQueue),
          m_messageQueue(messageQueue),
          m_pService(pService)
    {

    }

    void OnTimeOut(unsigned int timerID)
    {
    	try
    	{
        	m_pService->submitConsumeRequest(m_msgs, m_pProcessQueue, m_messageQueue, true);
        }
        catch(...)
        {
        	RMQ_ERROR("SubmitConsumeRequestLater OnTimeOut exception");
        }

        delete this;
    }

private:
    std::list<MessageExt*> m_msgs;
    ProcessQueue* m_pProcessQueue;
    MessageQueue m_messageQueue;
    ConsumeMessageConcurrentlyService* m_pService;
};

void ConsumeMessageConcurrentlyService::submitConsumeRequestLater(std::list<MessageExt*>& msgs,
        ProcessQueue* pProcessQueue,
        const MessageQueue& messageQueue)
{
    SubmitConsumeRequestLater* sc = new SubmitConsumeRequestLater(msgs, pProcessQueue, messageQueue, this);
    m_scheduledExecutorService->RegisterTimer(0, 5000, sc, false);
}

void ConsumeMessageConcurrentlyService::submitConsumeRequest(std::list<MessageExt*>& msgs,
        ProcessQueue* pProcessQueue,
        const MessageQueue& messageQueue,
        bool dispathToConsume)
{
	size_t consumeBatchSize = m_pDefaultMQPushConsumer->getConsumeMessageBatchMaxSize();

	RMQ_DEBUG("submitConsumeRequest begin, msgs.size=%d, messageQueue=%s, consumeBatchSize=%d, dispathToConsume=%d",
		(int)msgs.size(), messageQueue.toString().c_str(), (int)consumeBatchSize, dispathToConsume
    );

    if (msgs.size() <= consumeBatchSize)
    {
        kpr::ThreadPoolWorkPtr consumeRequest = new ConsumeConcurrentlyRequest(msgs, pProcessQueue, messageQueue, this);
        m_pConsumeExecutor->AddWork(consumeRequest);
    }
    else
    {
        std::list<MessageExt*>::iterator it = msgs.begin();
        for (; it != msgs.end();)
        {
            std::list<MessageExt*> msgThis;
            for (size_t i = 0; i < consumeBatchSize; i++, it++)
            {
                if (it != msgs.end())
                {
                    msgThis.push_back(*it);
                }
                else
                {
                    break;
                }
            }

            kpr::ThreadPoolWorkPtr consumeRequest = new ConsumeConcurrentlyRequest(msgThis, pProcessQueue, messageQueue, this);
            m_pConsumeExecutor->AddWork(consumeRequest);
        }
    }

    RMQ_DEBUG("submitConsumeRequest end");
}

void ConsumeMessageConcurrentlyService::updateCorePoolSize(int corePoolSize)
{
	//todo ��ʱ��֧�ֵ����̳߳ش�С
}

void ConsumeMessageConcurrentlyService::processConsumeResult(ConsumeConcurrentlyStatus status,
        ConsumeConcurrentlyContext& context,
        ConsumeConcurrentlyRequest& consumeRequest)
{
    int ackIndex = context.ackIndex;

    if (consumeRequest.getMsgs().empty())
    {
        return;
    }

    int msgsSize = consumeRequest.getMsgs().size();

    switch (status)
    {
        case CONSUME_SUCCESS:
        {
            if (ackIndex >= msgsSize)
            {
                ackIndex = msgsSize - 1;
            }

            int ok = ackIndex + 1;
            int failed = msgsSize - ok;
            // ͳ����Ϣ
            getConsumerStat().consumeMsgOKTotal.fetchAndAdd(ok);
            getConsumerStat().consumeMsgFailedTotal.fetchAndAdd(failed);
        }

        break;
        case RECONSUME_LATER:
            ackIndex = -1;
            // ͳ����Ϣ
            getConsumerStat().consumeMsgFailedTotal.fetchAndAdd(msgsSize);
            break;
        default:
            break;
    }

    std::list<MessageExt*>& msgs = consumeRequest.getMsgs();
    std::list<MessageExt*>::iterator it = msgs.begin();

    //�����Ѿ����ѵ���Ϣ
    for (int i = 0; i < ackIndex + 1 && it != msgs.end(); i++)
    {
        it++;
    }

    switch (m_pDefaultMQPushConsumer->getMessageModel())
    {
        case BROADCASTING:
            // ����ǹ㲥ģʽ��ֱ�Ӷ���ʧ����Ϣ����Ҫ���ĵ��и�֪�û�
            // ��������ԭ�򣺹㲥ģʽ����ʧ�����Դ��۹��ߣ���������Ⱥ���ܻ��нϴ�Ӱ�죬ʧ�����Թ��ܽ���Ӧ�ô���
            for (; it != msgs.end(); it++)
            {
                MessageExt* msg = *it;
                RMQ_WARN("BROADCASTING, the message consume failed, drop it, %s", msg->toString().c_str());
            }
            break;
        case CLUSTERING:
        {
            // ��������ʧ�ܵ���Ϣ��ֱ�ӷ��ص�Broker
            std::list<MessageExt*> msgBackFailed;
            for (; it != msgs.end(); it++)
            {
                MessageExt* msg = *it;
                bool result = sendMessageBack(*msg, context);
                if (!result)
                {
                    msg->setReconsumeTimes(msg->getReconsumeTimes() + 1);
                    msgBackFailed.push_back(msg);
                }
            }

            if (!msgBackFailed.empty())
            {
                // ����ʧ�ܵ���Ϣ��ȻҪ����
                // ɾ��consumeRequest�з���ʧ�ܵ���Ϣ
                it = msgs.begin();

                for (; it != msgs.end();)
                {
                    bool find = false;
                    std::list<MessageExt*>::iterator itFailed = msgBackFailed.begin();
                    for (; itFailed != msgBackFailed.end(); itFailed++)
                    {
                        if (*it == *itFailed)
                        {
                            it = msgs.erase(it);
                            find = true;
                            break;
                        }
                    }

                    if (!find)
                    {
                        it++;
                    }
                }

                // �˹��̴���ʧ�ܵ���Ϣ����Ҫ��Client������ʱ���ѣ�ֱ���ɹ�
                submitConsumeRequestLater(msgBackFailed, consumeRequest.getProcessQueue(),
                                          consumeRequest.getMessageQueue());
            }
        }
        break;
        default:
            break;
    }

    long long offset = consumeRequest.getProcessQueue()->removeMessage(consumeRequest.getMsgs());
    if (offset >= 0)
    {
        m_pDefaultMQPushConsumerImpl->getOffsetStore()->updateOffset(consumeRequest.getMessageQueue(),
                offset, true);
    }
}

std::string& ConsumeMessageConcurrentlyService::getConsumerGroup()
{
    return m_consumerGroup;
}

MessageListenerConcurrently* ConsumeMessageConcurrentlyService::getMessageListener()
{
    return m_pMessageListener;
}

DefaultMQPushConsumerImpl* ConsumeMessageConcurrentlyService::getDefaultMQPushConsumerImpl()
{
    return m_pDefaultMQPushConsumerImpl;
}

ConsumeConcurrentlyRequest::ConsumeConcurrentlyRequest(std::list<MessageExt*>& msgs,
        ProcessQueue* pProcessQueue,
        const MessageQueue& messageQueue,
        ConsumeMessageConcurrentlyService* pService)
{
	m_msgs = msgs;
	m_pProcessQueue = pProcessQueue;
	m_pService = pService;
    m_messageQueue = messageQueue;
}

ConsumeConcurrentlyRequest::~ConsumeConcurrentlyRequest()
{
	m_msgs.clear();
}

void ConsumeConcurrentlyRequest::Do()
{
	RMQ_DEBUG("consumeMessage begin, m_msgs.size=%d", (int)m_msgs.size());

    if (m_pProcessQueue->isDropped())
    {
        RMQ_INFO("the message queue not be able to consume, because it's droped, {%s}",
        	m_messageQueue.toString().c_str());
        return;
    }

	try
	{
	    MessageListenerConcurrently* listener = m_pService->getMessageListener();
	    ConsumeConcurrentlyContext context(m_messageQueue);
	    ConsumeConcurrentlyStatus status = RECONSUME_LATER;

	    // ִ��Hook
	    ConsumeMessageContext consumeMessageContext;
	    if (m_pService->getDefaultMQPushConsumerImpl()->hasHook())
	    {
	        consumeMessageContext.consumerGroup = m_pService->getConsumerGroup();
	        consumeMessageContext.mq = m_messageQueue;
	        consumeMessageContext.msgList = m_msgs;
	        consumeMessageContext.success = false;
	        m_pService->getDefaultMQPushConsumerImpl()->executeHookBefore(consumeMessageContext);
	    }

	    long long beginTimestamp = KPRUtil::GetCurrentTimeMillis();
	    try
	    {
	        resetRetryTopic(m_msgs);
	        status = listener->consumeMessage(m_msgs, context);
	    }
	    catch (...)
	    {
	        RMQ_WARN("consumeMessage exception, Group: {%s} Msgs: {%d} MQ: {%s}",
	        	m_pService->getConsumerGroup().c_str(),
	        	(int)m_msgs.size(),
	        	m_messageQueue.toString().c_str()
	        );
	    }

	    long long consumeRT = KPRUtil::GetCurrentTimeMillis() - beginTimestamp;

	    // ִ��Hook
	    if (m_pService->getDefaultMQPushConsumerImpl()->hasHook())
	    {
	        consumeMessageContext.success = (status == CONSUME_SUCCESS);
	        m_pService->getDefaultMQPushConsumerImpl()->executeHookAfter(consumeMessageContext);
	    }

	    // ��¼ͳ����Ϣ
	    m_pService->getConsumerStat().consumeMsgRTTotal.fetchAndAdd(consumeRT);
	    bool updated = MixAll::compareAndIncreaseOnly(m_pService->getConsumerStat().consumeMsgRTMax, consumeRT);
	    // ��ʱ���ֵ�¼�¼
	    if (updated)
	    {
	        RMQ_WARN("consumeMessage RT new max: %lld, Group: %s, Msgs: %d, MQ: %s",
	        	consumeRT,
	        	m_pService->getConsumerGroup().c_str(),
	        	(int)m_msgs.size(),
	        	m_messageQueue.toString().c_str()
	        );
	    }

		if (!m_pProcessQueue->isDropped())
		{
	    	m_pService->processConsumeResult(status, context, *this);
	    }
	    else
	    {
	    	RMQ_WARN("processQueue is dropped without process consume result, messageQueue={%s}, msgs.size={%d}",
	        	m_messageQueue.toString().c_str(), (int)m_msgs.size());
	    }
	}
	catch(...)
	{
		RMQ_WARN("ConsumeConcurrentlyRequest exception");
	}
	RMQ_DEBUG("consumeMessage end, m_msgs.size=%d", (int)m_msgs.size());

    return;
}

void ConsumeConcurrentlyRequest::resetRetryTopic(std::list<MessageExt*>& msgs)
{
    std::string groupTopic = MixAll::getRetryTopic(m_pService->getConsumerGroup());
    std::list<MessageExt*>::iterator it = msgs.begin();

    for (; it != msgs.end(); it++)
    {
        MessageExt* msg = (*it);
        std::string retryTopic = msg->getProperty(Message::PROPERTY_RETRY_TOPIC);
        if (!retryTopic.empty() && groupTopic == msg->getTopic())
        {
            msg->setTopic(retryTopic);
        }
    }
}

}