#pragma once
#include <ydb/core/tx/columnshard/counters/scan.h>
#include <ydb/core/tx/columnshard/engines/reader/abstract/read_metadata.h>
#include <ydb/core/tx/columnshard/engines/reader/common/conveyor_task.h>
#include <ydb/core/tx/columnshard/engines/reader/common_reader/iterator/columns_set.h>
#include <ydb/core/tx/columnshard/engines/reader/common_reader/iterator/fetching.h>
#include <ydb/core/tx/columnshard/engines/reader/simple_reader/duplicates/events.h>
#include <ydb/core/tx/columnshard/engines/scheme/abstract_scheme.h>
#include <ydb/core/tx/columnshard/engines/scheme/index_info.h>
#include <ydb/core/tx/limiter/grouped_memory/usage/abstract.h>

#include <ydb/library/accessor/accessor.h>

namespace NKikimr::NOlap::NReader::NSimple {

class IDataSource;
using TColumnsSet = NCommon::TColumnsSet;
using TIndexesSet = NCommon::TIndexesSet;
using TColumnsSetIds = NCommon::TColumnsSetIds;
using EMemType = NCommon::EMemType;
using TFetchingScriptCursor = NCommon::TFetchingScriptCursor;
using TStepAction = NCommon::TStepAction;


class IFetchingStep: public NCommon::IFetchingStep {
private:
    using TBase = NCommon::IFetchingStep;
    virtual TConclusion<bool> DoExecuteInplace(const std::shared_ptr<IDataSource>& source, const TFetchingScriptCursor& step) const = 0;

    virtual ui64 GetProcessingDataSize(const std::shared_ptr<IDataSource>& /*source*/) const {
        return 0;
    }

    virtual TConclusion<bool> DoExecuteInplace(
        const std::shared_ptr<NCommon::IDataSource>& sourceExt, const TFetchingScriptCursor& step) const override final;

    virtual ui64 GetProcessingDataSize(const std::shared_ptr<NCommon::IDataSource>& source) const override final;

public:
    using TBase::TBase;
};

class IDataSource;

class TDetectInMemStep: public IFetchingStep {
private:
    using TBase = IFetchingStep;
    const TColumnsSetIds Columns;

protected:
    virtual TConclusion<bool> DoExecuteInplace(const std::shared_ptr<IDataSource>& source, const TFetchingScriptCursor& step) const override;
    virtual TString DoDebugString() const override {
        return TStringBuilder() << "columns=" << Columns.DebugString() << ";";
    }

public:
    virtual ui64 GetProcessingDataSize(const std::shared_ptr<IDataSource>& source) const override;
    TDetectInMemStep(const TColumnsSetIds& columns)
        : TBase("FETCHING_COLUMNS")
        , Columns(columns) {
        AFL_VERIFY(Columns.GetColumnsCount());
    }
};

class TPrepareResultStep: public IFetchingStep {
private:
    using TBase = IFetchingStep;

protected:
    virtual TConclusion<bool> DoExecuteInplace(const std::shared_ptr<IDataSource>& source, const TFetchingScriptCursor& step) const override;
    virtual TString DoDebugString() const override {
        return TStringBuilder();
    }

public:
    virtual ui64 GetProcessingDataSize(const std::shared_ptr<IDataSource>& /*source*/) const override {
        return 0;
    }
    TPrepareResultStep()
        : TBase("PREPARE_RESULT") {
    }
};

class TBuildResultStep: public IFetchingStep {
private:
    using TBase = IFetchingStep;
    const ui32 StartIndex;
    const ui32 RecordsCount;

protected:
    virtual TConclusion<bool> DoExecuteInplace(const std::shared_ptr<IDataSource>& source, const TFetchingScriptCursor& step) const override;
    virtual TString DoDebugString() const override {
        return TStringBuilder();
    }

public:
    virtual ui64 GetProcessingDataSize(const std::shared_ptr<IDataSource>& /*source*/) const override {
        return 0;
    }
    TBuildResultStep(const ui32 startIndex, const ui32 recordsCount)
        : TBase("BUILD_RESULT")
        , StartIndex(startIndex)
        , RecordsCount(recordsCount) {
    }
};

class TColumnBlobsFetchingStep: public IFetchingStep {
private:
    using TBase = IFetchingStep;
    TColumnsSetIds Columns;

protected:
    virtual TConclusion<bool> DoExecuteInplace(const std::shared_ptr<IDataSource>& source, const TFetchingScriptCursor& step) const override;
    virtual TString DoDebugString() const override {
        return TStringBuilder() << "columns=" << Columns.DebugString() << ";";
    }

public:
    virtual ui64 GetProcessingDataSize(const std::shared_ptr<IDataSource>& source) const override;
    TColumnBlobsFetchingStep(const TColumnsSetIds& columns)
        : TBase("FETCHING_COLUMNS")
        , Columns(columns) {
        AFL_VERIFY(Columns.GetColumnsCount());
    }
};

class TPortionAccessorFetchingStep: public IFetchingStep {
private:
    using TBase = IFetchingStep;

protected:
    virtual TConclusion<bool> DoExecuteInplace(const std::shared_ptr<IDataSource>& source, const TFetchingScriptCursor& step) const override;
    virtual TString DoDebugString() const override {
        return TStringBuilder();
    }

public:
    TPortionAccessorFetchingStep()
        : TBase("FETCHING_ACCESSOR") {
    }
};

class TFilterCutLimit: public IFetchingStep {
private:
    using TBase = IFetchingStep;
    const ui32 Limit;
    const bool Reverse;

public:
    virtual TConclusion<bool> DoExecuteInplace(const std::shared_ptr<IDataSource>& source, const TFetchingScriptCursor& step) const override;
    TFilterCutLimit(const ui32 limit, const bool reverse)
        : TBase("LIMIT")
        , Limit(limit)
        , Reverse(reverse) {
        AFL_VERIFY(Limit);
    }
};

class TPredicateFilter: public IFetchingStep {
private:
    using TBase = IFetchingStep;

public:
    virtual TConclusion<bool> DoExecuteInplace(const std::shared_ptr<IDataSource>& source, const TFetchingScriptCursor& step) const override;
    TPredicateFilter()
        : TBase("PREDICATE") {
    }
};

class TSnapshotFilter: public IFetchingStep {
private:
    using TBase = IFetchingStep;

public:
    virtual TConclusion<bool> DoExecuteInplace(const std::shared_ptr<IDataSource>& source, const TFetchingScriptCursor& step) const override;
    TSnapshotFilter()
        : TBase("SNAPSHOT") {
    }
};

class TDetectInMem: public IFetchingStep {
private:
    using TBase = IFetchingStep;
    TColumnsSetIds Columns;

public:
    virtual TConclusion<bool> DoExecuteInplace(const std::shared_ptr<IDataSource>& source, const TFetchingScriptCursor& step) const override;
    TDetectInMem(const TColumnsSetIds& columns)
        : TBase("DETECT_IN_MEM")
        , Columns(columns) {
    }
};

class TDeletionFilter: public IFetchingStep {
private:
    using TBase = IFetchingStep;

public:
    virtual TConclusion<bool> DoExecuteInplace(const std::shared_ptr<IDataSource>& source, const TFetchingScriptCursor& step) const override;
    TDeletionFilter()
        : TBase("DELETION") {
    }
};

class TShardingFilter: public IFetchingStep {
private:
    using TBase = IFetchingStep;

public:
    virtual TConclusion<bool> DoExecuteInplace(const std::shared_ptr<IDataSource>& source, const TFetchingScriptCursor& step) const override;
    TShardingFilter()
        : TBase("SHARDING") {
    }
};

class TDuplicateFilter: public IFetchingStep {
private:
    using TBase = IFetchingStep;

    class TFilterSubscriber: public NDuplicateFiltering::IFilterSubscriber {
    private:
        std::weak_ptr<IDataSource> Source;
        TFetchingScriptCursor Step;
        NColumnShard::TCounterGuard TaskGuard;

        virtual void OnFilterReady(NArrow::TColumnFilter&& filter) override;
        virtual void OnFailure(const TString& reason) override;

    public:
        TFilterSubscriber(const std::shared_ptr<IDataSource>& source, const TFetchingScriptCursor& step);
    };

public:
    virtual TConclusion<bool> DoExecuteInplace(const std::shared_ptr<IDataSource>& source, const TFetchingScriptCursor& step) const override;
    TDuplicateFilter()
        : TBase("DUPLICATE") {
    }
};

}   // namespace NKikimr::NOlap::NReader::NSimple
