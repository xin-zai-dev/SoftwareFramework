#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QTcpSocket>

namespace ivs {

// Blocking API: create and use on the same worker thread, never on the UI thread.
class ModbusTcpClient final {
  public:
    ModbusTcpClient() = default;
    ~ModbusTcpClient();
    ModbusTcpClient(const ModbusTcpClient &) = delete;
    ModbusTcpClient &operator=(const ModbusTcpClient &) = delete;

    bool connectToHost(const QString &host, int port, int unitId = 1, int timeoutMs = 1000,
                       QString *error = nullptr);
    void disconnectFromHost();
    QList<quint16> readHoldingRegisters(quint16 address, quint16 count, QString *error = nullptr);
    QList<quint16> readInputRegisters(quint16 address, quint16 count, QString *error = nullptr);
    QList<bool> readCoils(quint16 address, quint16 count, QString *error = nullptr);
    QList<bool> readDiscreteInputs(quint16 address, quint16 count, QString *error = nullptr);
    bool writeSingleRegister(quint16 address, quint16 value, QString *error = nullptr);
    bool writeSingleCoil(quint16 address, bool value, QString *error = nullptr);

  private:
    QByteArray transact(quint8 function, const QByteArray &payload, QString *error);
    QList<quint16> readRegisters(quint8 function, quint16 address, quint16 count, QString *error);
    QList<bool> readBits(quint8 function, quint16 address, quint16 count, QString *error);
    bool writeSingle(quint8 function, quint16 address, quint16 value, QString *error);

    QTcpSocket m_socket;
    quint16 m_transaction = 0;
    quint8 m_unitId = 1;
    int m_timeoutMs = 1000;
};

} // namespace ivs
