#include "ModbusTcpClient.h"

#include <QDeadlineTimer>

namespace ivs {

namespace {
void clearError(QString *error) {
    if (error)
        error->clear();
}
void setError(QString *error, const QString &message) {
    if (error)
        *error = message;
}

void appendWord(QByteArray &data, quint16 value) {
    data.append(char(value >> 8));
    data.append(char(value & 0xff));
}

quint16 wordAt(const QByteArray &data, qsizetype offset) {
    return (quint16(quint8(data.at(offset))) << 8) | quint8(data.at(offset + 1));
}

QString exceptionName(quint8 code) {
    switch (code) {
    case 1:
        return QStringLiteral("illegal function");
    case 2:
        return QStringLiteral("illegal data address");
    case 3:
        return QStringLiteral("illegal data value");
    case 4:
        return QStringLiteral("server device failure");
    case 5:
        return QStringLiteral("acknowledge");
    case 6:
        return QStringLiteral("server device busy");
    case 10:
        return QStringLiteral("gateway path unavailable");
    case 11:
        return QStringLiteral("gateway target failed to respond");
    default:
        return QStringLiteral("device-specific exception");
    }
}
} // namespace

ModbusTcpClient::~ModbusTcpClient() {
    disconnectFromHost();
}

bool ModbusTcpClient::connectToHost(const QString &host, int port, int unitId, int timeoutMs,
                                    QString *error) {
    clearError(error);
    disconnectFromHost();
    if (host.trimmed().isEmpty() || port < 1 || port > 65535 || unitId < 0 || unitId > 255 || timeoutMs < 1 ||
        timeoutMs > 60000) {
        setError(error,
                 QStringLiteral("Invalid Modbus host, port, unit ID (0..255), or timeout (1..60000 ms)"));
        return false;
    }
    m_unitId = quint8(unitId);
    m_timeoutMs = timeoutMs;
    m_socket.connectToHost(host, quint16(port));
    if (!m_socket.waitForConnected(m_timeoutMs)) {
        setError(error, QStringLiteral("Modbus connection failed: %1").arg(m_socket.errorString()));
        m_socket.abort();
        return false;
    }
    return true;
}

void ModbusTcpClient::disconnectFromHost() {
    m_socket.abort();
}

QByteArray ModbusTcpClient::transact(quint8 function, const QByteArray &payload, QString *error) {
    clearError(error);
    if (m_socket.state() != QAbstractSocket::ConnectedState) {
        setError(error, QStringLiteral("Modbus device is not connected"));
        return {};
    }
    const quint16 transaction = ++m_transaction;
    QByteArray request;
    appendWord(request, transaction);
    appendWord(request, 0);                           // MBAP protocol identifier.
    appendWord(request, quint16(payload.size() + 2)); // unit + function + payload.
    request.append(char(m_unitId));
    request.append(char(function));
    request.append(payload);
    QDeadlineTimer deadline(m_timeoutMs);
    auto fail = [&](const QString &message) -> QByteArray {
        setError(error, message);
        // Framing or timeout errors make a delayed response unsafe for the next request.
        m_socket.abort();
        return {};
    };
    if (m_socket.write(request) != request.size())
        return fail(QStringLiteral("Modbus request write failed: %1").arg(m_socket.errorString()));
    while (m_socket.bytesToWrite()) {
        if (deadline.hasExpired() || !m_socket.waitForBytesWritten(int(deadline.remainingTime())))
            return fail(
                QStringLiteral("Modbus write timeout or connection error: %1").arg(m_socket.errorString()));
    }

    auto readExactly = [&](qsizetype count) {
        QByteArray bytes;
        while (bytes.size() < count) {
            bytes.append(m_socket.read(count - bytes.size()));
            if (bytes.size() == count)
                break;
            if (deadline.hasExpired() || !m_socket.waitForReadyRead(int(deadline.remainingTime())))
                break;
        }
        return bytes;
    };

    const QByteArray header = readExactly(7);
    if (header.size() != 7)
        return fail(QStringLiteral("Modbus response timeout or truncated MBAP header"));
    const quint16 length = wordAt(header, 4);
    if (wordAt(header, 0) != transaction)
        return fail(QStringLiteral("Modbus response transaction ID mismatch"));
    if (wordAt(header, 2) != 0)
        return fail(QStringLiteral("Modbus response protocol ID must be zero"));
    if (quint8(header.at(6)) != m_unitId)
        return fail(QStringLiteral("Modbus response unit ID mismatch"));
    if (length < 2 || length > 254)
        return fail(QStringLiteral("Modbus response MBAP length is invalid"));
    const QByteArray pdu = readExactly(length - 1);
    if (pdu.size() != length - 1)
        return fail(QStringLiteral("Modbus response timeout or truncated PDU"));
    const quint8 receivedFunction = quint8(pdu.at(0));
    if (receivedFunction == quint8(function | 0x80)) {
        if (pdu.size() != 2)
            return fail(QStringLiteral("Malformed Modbus exception response"));
        setError(error, QStringLiteral("Modbus exception 0x%1: %2")
                            .arg(quint8(pdu.at(1)), 2, 16, QLatin1Char('0'))
                            .arg(exceptionName(quint8(pdu.at(1)))));
        return {};
    }
    if (receivedFunction != function)
        return fail(QStringLiteral("Modbus response function mismatch"));
    return pdu;
}

QList<quint16> ModbusTcpClient::readRegisters(quint8 function, quint16 address, quint16 count,
                                              QString *error) {
    clearError(error);
    if (count < 1 || count > 125 || quint32(address) + count > 65536) {
        setError(error,
                 QStringLiteral("Modbus register read requires 1..125 registers within address 0..65535"));
        return {};
    }
    QByteArray payload;
    appendWord(payload, address);
    appendWord(payload, count);
    const QByteArray pdu = transact(function, payload, error);
    if (pdu.isEmpty())
        return {};
    if (pdu.size() != 2 + count * 2 || quint8(pdu.at(1)) != count * 2) {
        setError(error, QStringLiteral("Modbus register response byte count mismatch"));
        m_socket.abort();
        return {};
    }
    QList<quint16> values;
    values.reserve(count);
    for (quint16 i = 0; i < count; ++i)
        values.append(wordAt(pdu, 2 + i * 2));
    return values;
}

QList<bool> ModbusTcpClient::readBits(quint8 function, quint16 address, quint16 count, QString *error) {
    clearError(error);
    if (count < 1 || count > 2000 || quint32(address) + count > 65536) {
        setError(error, QStringLiteral("Modbus bit read requires 1..2000 bits within address 0..65535"));
        return {};
    }
    QByteArray payload;
    appendWord(payload, address);
    appendWord(payload, count);
    const QByteArray pdu = transact(function, payload, error);
    if (pdu.isEmpty())
        return {};
    const int byteCount = (count + 7) / 8;
    if (pdu.size() != 2 + byteCount || quint8(pdu.at(1)) != byteCount) {
        setError(error, QStringLiteral("Modbus bit response byte count mismatch"));
        m_socket.abort();
        return {};
    }
    QList<bool> values;
    values.reserve(count);
    for (quint16 i = 0; i < count; ++i)
        values.append((quint8(pdu.at(2 + i / 8)) & (1u << (i % 8))) != 0);
    return values;
}

bool ModbusTcpClient::writeSingle(quint8 function, quint16 address, quint16 value, QString *error) {
    QByteArray payload;
    appendWord(payload, address);
    appendWord(payload, value);
    const QByteArray pdu = transact(function, payload, error);
    if (pdu.isEmpty())
        return false;
    if (pdu.size() != 5 || pdu.mid(1) != payload) {
        setError(error, QStringLiteral("Modbus write response does not echo address and value"));
        m_socket.abort();
        return false;
    }
    return true;
}

QList<quint16> ModbusTcpClient::readHoldingRegisters(quint16 address, quint16 count, QString *error) {
    return readRegisters(3, address, count, error);
}
QList<quint16> ModbusTcpClient::readInputRegisters(quint16 address, quint16 count, QString *error) {
    return readRegisters(4, address, count, error);
}
QList<bool> ModbusTcpClient::readCoils(quint16 address, quint16 count, QString *error) {
    return readBits(1, address, count, error);
}
QList<bool> ModbusTcpClient::readDiscreteInputs(quint16 address, quint16 count, QString *error) {
    return readBits(2, address, count, error);
}
bool ModbusTcpClient::writeSingleRegister(quint16 address, quint16 value, QString *error) {
    return writeSingle(6, address, value, error);
}
bool ModbusTcpClient::writeSingleCoil(quint16 address, bool value, QString *error) {
    return writeSingle(5, address, value ? 0xff00 : 0, error);
}

} // namespace ivs
