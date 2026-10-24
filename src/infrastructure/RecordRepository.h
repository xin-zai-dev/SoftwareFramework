#pragma once

#include "domain/Types.h"

#include <atomic>
#include <functional>
#include <memory>

namespace ivs {

// A repository and its SQL connection belong to the thread that calls open().
class RecordRepository final {
  public:
    RecordRepository();
    ~RecordRepository();
    RecordRepository(const RecordRepository &) = delete;
    RecordRepository &operator=(const RecordRepository &) = delete;

    bool open(const DatabaseConfig &config, const QString &workspace, QString *error = nullptr);
    bool append(DetectionRecord &record, QString *error = nullptr);
    QList<DetectionRecord> query(const RecordFilter &filter, QString *error = nullptr) const;
    qint64 count(const RecordFilter &filter = {}, QString *error = nullptr) const;
    RecordSummary summary(QString *error = nullptr) const;
    bool exportCsv(const QString &path, const RecordFilter &filter, QString *error = nullptr) const;
    bool seed(qint64 count, QString *error = nullptr, std::function<void(qint64)> progress = {},
              const std::atomic_bool *cancel = nullptr);
    QString databasePath() const;

  private:
    class Private;
    std::unique_ptr<Private> d;
};

} // namespace ivs
