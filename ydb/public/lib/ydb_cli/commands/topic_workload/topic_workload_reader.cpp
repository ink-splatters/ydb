#include "topic_workload_reader.h"

#include "topic_workload_describe.h"

#include <ydb/public/sdk/cpp/client/ydb_topic/topic.h>
#include <ydb/public/lib/ydb_cli/commands/ydb_common.h>

using namespace NYdb::NConsoleClient;

void TTopicWorkloadReader::ReaderLoop(TTopicWorkloadReaderParams& params) {
    auto topicClient = std::make_unique<NYdb::NTopic::TTopicClient>(params.Driver);

    auto consumerName = TCommandWorkloadTopicDescribe::GenerateConsumerName(params.ConsumerIdx);
    auto describeTopicResult = TCommandWorkloadTopicDescribe::DescribeTopic(params.Database, params.TopicName, params.Driver);
    auto consumers = describeTopicResult.GetConsumers();
    if (!std::any_of(consumers.begin(), consumers.end(), [consumerName](const auto& consumer) { return consumer.GetConsumerName() == consumerName; }))
    {
        WRITE_LOG(params.Log, ELogPriority::TLOG_EMERG, TStringBuilder() << "Topic '" << params.TopicName << "' doesn't have a consumer '" << consumerName << "'. Run command 'workload init' with parameter '--consumers'.");
        exit(EXIT_FAILURE);
    }
    NYdb::NTopic::TReadSessionSettings settings;
    settings.ConsumerName(consumerName).AppendTopics(params.TopicName);

    auto readSession = topicClient->CreateReadSession(settings);
    WRITE_LOG(params.Log, ELogPriority::TLOG_INFO, "Reader session was created.");

    struct TPartitionStreamState {
        ui64 StartOffset;
        NYdb::NTopic::TPartitionSession::TPtr Stream;
    };
    THashMap<std::pair<TString, ui64>, TPartitionStreamState> streamState;

    TInstant LastPartitionStatusRequestTime = TInstant::Zero();

    (*params.StartedCount)++;

    const TInstant endTime = Now() + TDuration::Seconds(params.TotalSec + 3);

    while (Now() < endTime && !*params.ErrorFlag) {
        TInstant st = TInstant::Now();
        if (TInstant::Now() - LastPartitionStatusRequestTime > TDuration::Seconds(1)) {
            for (auto& st : streamState) {
                if (st.second.Stream) {
                    st.second.Stream->RequestStatus();
                }
            }
            LastPartitionStatusRequestTime = st;
        }

        readSession->WaitEvent().Wait(TDuration::Seconds(1));
        auto events = readSession->GetEvents(false);

        auto now = TInstant::Now();
        for (auto& event : events) {
            if (auto* dataEvent = std::get_if<NYdb::NTopic::TReadSessionEvent::TDataReceivedEvent>(&event)) {
                WRITE_LOG(params.Log, ELogPriority::TLOG_DEBUG, TStringBuilder() << dataEvent->DebugString());

                for (const auto& message : dataEvent->GetMessages()) {
                    ui64 fullTime = (now - message.GetCreateTime()).MilliSeconds();
                    params.StatsCollector->AddReaderEvent(params.ReaderIdx, {message.GetData().Size(), fullTime});

                    WRITE_LOG(params.Log, ELogPriority::TLOG_DEBUG, TStringBuilder() << "Got message: " << message.GetMessageGroupId() 
                        << " topic " << message.GetPartitionSession()->GetTopicPath() << " partition " << message.GetPartitionSession()->GetPartitionId() 
                        << " offset " << message.GetOffset() << " seqNo " << message.GetSeqNo()
                        << " createTime " << message.GetCreateTime() << " fullTimeMs " << fullTime);
                }
                dataEvent->Commit();
            } else if (auto* createPartitionStreamEvent = std::get_if<NYdb::NTopic::TReadSessionEvent::TStartPartitionSessionEvent>(&event)) {
                auto stream = createPartitionStreamEvent->GetPartitionSession();
                ui64 startOffset = streamState[std::make_pair(stream->GetTopicPath(), stream->GetPartitionId())].StartOffset;
                streamState[std::make_pair(stream->GetTopicPath(), stream->GetPartitionId())].Stream = stream;
                WRITE_LOG(params.Log, ELogPriority::TLOG_DEBUG, TStringBuilder() << "Starting read " << createPartitionStreamEvent->DebugString() << " from " << startOffset);
                createPartitionStreamEvent->Confirm();
            } else if (auto* destroyPartitionStreamEvent = std::get_if<NYdb::NTopic::TReadSessionEvent::TStopPartitionSessionEvent>(&event)) {
                auto stream = destroyPartitionStreamEvent->GetPartitionSession();
                streamState[std::make_pair(stream->GetTopicPath(), stream->GetPartitionId())].Stream = nullptr;
                destroyPartitionStreamEvent->Confirm();
            } else if (auto* closeSessionEvent = std::get_if<NYdb::NTopic::TSessionClosedEvent>(&event)) {
                WRITE_LOG(params.Log, ELogPriority::TLOG_ERR, TStringBuilder() << "Read session closed: " << closeSessionEvent->DebugString());
                *params.ErrorFlag = 1;
                break;
            } else if (auto* partitionStreamStatusEvent = std::get_if<NYdb::NTopic::TReadSessionEvent::TPartitionSessionStatusEvent>(&event)) {
                WRITE_LOG(params.Log, ELogPriority::TLOG_DEBUG, TStringBuilder() << partitionStreamStatusEvent->DebugString())

                ui64 lagMessages = partitionStreamStatusEvent->GetEndOffset() - partitionStreamStatusEvent->GetCommittedOffset();
                ui64 lagTime = lagMessages == 0 ? 0 : (now - partitionStreamStatusEvent->GetWriteTimeHighWatermark()).MilliSeconds();

                params.StatsCollector->AddLagEvent(params.ReaderIdx, {lagMessages, lagTime});
            } else if (auto* ackEvent = std::get_if<NYdb::NTopic::TReadSessionEvent::TCommitOffsetAcknowledgementEvent>(&event)) {
                WRITE_LOG(params.Log, ELogPriority::TLOG_DEBUG, TStringBuilder() << ackEvent->DebugString());
            }
        }
    }
}
