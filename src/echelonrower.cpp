#include "echelonrower.h"
#include "virtualbike.h"
#include "keepawakehelper.h"
#include <QFile>
#include <QDateTime>
#include <QMetaEnum>
#include <QSettings>
#include <QBluetoothLocalDevice>
#include <math.h>
#include "ios/lockscreen.h"

#ifdef Q_OS_IOS
extern quint8 QZ_EnableDiscoveryCharsAndDescripttors;
#endif

echelonrower::echelonrower(bool noWriteResistance, bool noHeartService, uint8_t bikeResistanceOffset, double bikeResistanceGain)
{
#ifdef Q_OS_IOS
    QZ_EnableDiscoveryCharsAndDescripttors = true;
#endif
    m_watt.setType(metric::METRIC_WATT);
    refresh = new QTimer(this);
    this->noWriteResistance = noWriteResistance;
    this->noHeartService = noHeartService;
    this->bikeResistanceGain = bikeResistanceGain;
    this->bikeResistanceOffset = bikeResistanceOffset;
    initDone = false;
    connect(refresh, SIGNAL(timeout()), this, SLOT(update()));
    refresh->start(200);
}

void echelonrower::writeCharacteristic(uint8_t* data, uint8_t data_len, QString info, bool disable_log, bool wait_for_response)
{
    QEventLoop loop;
    QTimer timeout;

    // if there are some crash here, maybe it's better to use 2 separate event for the characteristicChanged.
    // one for the resistance changed event (spontaneous), and one for the other ones.
    if(wait_for_response)
    {
        connect(gattCommunicationChannelService, SIGNAL(characteristicChanged(QLowEnergyCharacteristic,QByteArray)),
                &loop, SLOT(quit()));
        timeout.singleShot(300, &loop, SLOT(quit()));
    }
    else
    {
        connect(gattCommunicationChannelService, SIGNAL(characteristicWritten(QLowEnergyCharacteristic,QByteArray)),
                &loop, SLOT(quit()));
        timeout.singleShot(300, &loop, SLOT(quit()));
    }

    if(gattCommunicationChannelService->state() != QLowEnergyService::ServiceState::ServiceDiscovered ||
       m_control->state() == QLowEnergyController::UnconnectedState)
    {
        qDebug() << "writeCharacteristic error because the connection is closed";
        return;
    }

    if(!gattWriteCharacteristic.isValid())
    {
        qDebug() << "gattWriteCharacteristic is invalid";
        return;
    }

    gattCommunicationChannelService->writeCharacteristic(gattWriteCharacteristic, QByteArray((const char*)data, data_len));

    if(!disable_log)
        qDebug() << " >> " + QByteArray((const char*)data, data_len).toHex(' ') + " // " + info;

    loop.exec();
}

void echelonrower::forceResistance(int8_t requestResistance)
{
    uint8_t noOpData[] = { 0xf0, 0xb1, 0x01, 0x00, 0x00 };

    noOpData[3] = requestResistance;

    for(uint8_t i=0; i<sizeof(noOpData)-1; i++)
    {
       noOpData[4] += noOpData[i]; // the last byte is a sort of a checksum
    }

    writeCharacteristic(noOpData, sizeof(noOpData), "force resistance", false, true);
}

void echelonrower::sendPoll()
{
    uint8_t noOpData[] = { 0xf0, 0xa0, 0x01, 0x00, 0x00 };

    noOpData[3] = counterPoll;

    for(uint8_t i=0; i<sizeof(noOpData)-1; i++)
    {
       noOpData[4] += noOpData[i]; // the last byte is a sort of a checksum
    }

    writeCharacteristic(noOpData, sizeof(noOpData), "noOp", false, true);

    counterPoll++;
    if(!counterPoll)
        counterPoll = 1;
}

void echelonrower::update()
{
    if(m_control->state() == QLowEnergyController::UnconnectedState)
    {
        emit disconnected();
        return;
    }

    if(initRequest)
    {
        initRequest = false;
        btinit();
    }
    else if(bluetoothDevice.isValid() &&
       m_control->state() == QLowEnergyController::DiscoveredState &&
       gattCommunicationChannelService &&
       gattWriteCharacteristic.isValid() &&
       gattNotify1Characteristic.isValid() &&
       gattNotify2Characteristic.isValid() &&
       initDone)
    {
        update_metrics(true, watts());

        // sending poll every 2 seconds
        if(sec1Update++ >= (2000 / refresh->interval()))
        {
            sec1Update = 0;
            sendPoll();
            //updateDisplay(elapsed);
        }

        if(requestResistance != -1)
        {
           if(requestResistance > max_resistance) requestResistance = max_resistance;
           else if(requestResistance <= 0) requestResistance = 1;

           if(requestResistance != currentResistance().value())
           {
              qDebug() << "writing resistance " + QString::number(requestResistance);
              forceResistance(requestResistance);
           }
           requestResistance = -1;
        }
        if(requestStart != -1)
        {
           qDebug() << "starting...";

           //btinit();

           requestStart = -1;
           emit bikeStarted();
        }
        if(requestStop != -1)
        {
            qDebug() << "stopping...";
            //writeCharacteristic(initDataF0C800B8, sizeof(initDataF0C800B8), "stop tape");
            requestStop = -1;
        }
    }
}

void echelonrower::serviceDiscovered(const QBluetoothUuid &gatt)
{
    qDebug() << "serviceDiscovered " + gatt.toString();
}

int echelonrower::pelotonToBikeResistance(int pelotonResistance)
{
    for(int i = 1; i<max_resistance-1; i++)
    {
        if(bikeResistanceToPeloton(i) <= pelotonResistance && bikeResistanceToPeloton(i+1) >= pelotonResistance)
            return i;
    }
    return Resistance.value();
}

uint8_t echelonrower::resistanceFromPowerRequest(uint16_t power)
{
    qDebug() << "resistanceFromPowerRequest" << Cadence.value();

    for(int i = 1; i<max_resistance-1; i++)
    {
        if(wattsFromResistance(i) <= power && wattsFromResistance(i+1) >= power)
        {
            qDebug() << "resistanceFromPowerRequest" << wattsFromResistance(i) << wattsFromResistance(i+1) << power;
            return i;
        }
    }
    return Resistance.value();
}

double echelonrower::bikeResistanceToPeloton(double resistance)
{
    //0,0097x3 - 0,4972x2 + 10,126x - 37,08
    double p = ((pow(resistance,3) * 0.0097) - (0.4972 * pow(resistance, 2)) + (10.126 * resistance) - 37.08);
    if(p < 0)
        p = 0;
    return p;
}

void echelonrower::characteristicChanged(const QLowEnergyCharacteristic &characteristic, const QByteArray &newValue)
{
    //qDebug() << "characteristicChanged" << characteristic.uuid() << newValue << newValue.length();
    Q_UNUSED(characteristic);
    QSettings settings;
    QString heartRateBeltName = settings.value("heart_rate_belt_name", "Disabled").toString();

    qDebug() << " << " + newValue.toHex(' ');

    lastPacket = newValue;

    // resistance value is in another frame
    if(newValue.length() == 5 && ((unsigned char)newValue.at(0)) == 0xf0 && ((unsigned char)newValue.at(1)) == 0xd2)
    {
        Resistance = newValue.at(3);
        emit resistanceRead(Resistance.value());
        m_pelotonResistance = bikeResistanceToPeloton(Resistance.value());

        qDebug() << "Current resistance: " + QString::number(Resistance.value());
        return;
    }

    if (newValue.length() != 21)
        return;

    /*if ((uint8_t)(newValue.at(0)) != 0xf0 && (uint8_t)(newValue.at(1)) != 0xd1)
        return;*/

    double distance = GetDistanceFromPacket(newValue);

    if(settings.value("cadence_sensor_name", "Disabled").toString().startsWith("Disabled"))
        Cadence = ((uint8_t)newValue.at(11));
    Speed = (0.37497622 * ((double)Cadence.value())) / 2.0;
    KCal += ((( (0.048 * ((double)watts()) + 1.19) * settings.value("weight", 75.0).toFloat() * 3.5) / 200.0 ) / (60000.0 / ((double)lastRefreshCharacteristicChanged.msecsTo(QDateTime::currentDateTime())))); //(( (0.048* Output in watts +1.19) * body weight in kg * 3.5) / 200 ) / 60
    //Distance += ((Speed.value() / 3600000.0) * ((double)lastRefreshCharacteristicChanged.msecsTo(QDateTime::currentDateTime())) );
    Distance = distance;

    if(Cadence.value() > 0)
    {
        CrankRevs++;
        LastCrankEventTime += (uint16_t)(1024.0 / (((double)(Cadence.value())) / 60.0));
    }

    lastRefreshCharacteristicChanged = QDateTime::currentDateTime();

#ifdef Q_OS_ANDROID
    if(settings.value("ant_heart", false).toBool())
        Heart = (uint8_t)KeepAwakeHelper::heart();
    else
#endif
    {
        if(heartRateBeltName.startsWith("Disabled"))
        {
#ifdef Q_OS_IOS
#ifndef IO_UNDER_QT
            lockscreen h;
            long appleWatchHeartRate = h.heartRate();
            h.setKcal(KCal.value());
            h.setDistance(Distance.value());
            Heart = appleWatchHeartRate;
            qDebug() << "Current Heart from Apple Watch: " + QString::number(appleWatchHeartRate);
#endif
#endif
        }
    }

#ifdef Q_OS_IOS
#ifndef IO_UNDER_QT
    bool cadence = settings.value("bike_cadence_sensor", false).toBool();
    bool ios_peloton_workaround = settings.value("ios_peloton_workaround", true).toBool();
    if(ios_peloton_workaround && cadence && h && firstStateChanged)
    {
        h->virtualbike_setCadence(currentCrankRevolutions(),lastCrankEventTime());
        h->virtualbike_setHeartRate((uint8_t)metrics_override_heartrate());
    }
#endif
#endif

    qDebug() << "Current Local elapsed: " + GetElapsedFromPacket(newValue).toString();
    qDebug() << "Current Speed: " + QString::number(Speed.value());
    qDebug() << "Current Calculate Distance: " + QString::number(Distance.value());
    qDebug() << "Current Cadence: " + QString::number(Cadence.value());
    qDebug() << "Current Distance: " + QString::number(distance);
    qDebug() << "Current CrankRevs: " + QString::number(CrankRevs);
    qDebug() << "Last CrankEventTime: " + QString::number(LastCrankEventTime);
    qDebug() << "Current Watt: " + QString::number(watts());

    if(m_control->error() != QLowEnergyController::NoError)
        qDebug() << "QLowEnergyController ERROR!!" << m_control->errorString();
}

QTime echelonrower::GetElapsedFromPacket(QByteArray packet)
{
    uint16_t convertedData = (packet.at(3) << 8) | packet.at(4);
    QTime t(0,convertedData / 60, convertedData % 60);
    return t;
}

double echelonrower::GetDistanceFromPacket(QByteArray packet)
{
    uint32_t convertedData = (packet.at(15) << 16) | (packet.at(16) << 8) | packet.at(17);
    double data = ((double)convertedData) / 1000.0f;
    return data;
}

void echelonrower::btinit()
{
    uint8_t initData1[] = { 0xf0, 0xa1, 0x00, 0x91 };
    uint8_t initData2[] = { 0xf0, 0xa3, 0x00, 0x93 };
    uint8_t initData3[] = { 0xf0, 0xb0, 0x01, 0x01, 0xa2 };
    //uint8_t initData4[] = { 0xf0, 0x60, 0x00, 0x50 }; // get sleep command

    // useless i guess
    //writeCharacteristic(initData4, sizeof(initData4), "get sleep", false, true);

    // in the snoof log it repeats this frame 4 times, i will have to analyze the response to understand if 4 times are enough
    writeCharacteristic(initData1, sizeof(initData1), "init", false, true);
    writeCharacteristic(initData1, sizeof(initData1), "init", false, true);
    writeCharacteristic(initData1, sizeof(initData1), "init", false, true);
    writeCharacteristic(initData1, sizeof(initData1), "init", false, true);

    writeCharacteristic(initData2, sizeof(initData2), "init", false, true);
    writeCharacteristic(initData1, sizeof(initData1), "init", false, true);
    writeCharacteristic(initData3, sizeof(initData3), "init", false, true);

    initDone = true;

    if(lastResistanceBeforeDisconnection != -1)
    {
        qDebug() << "forcing resistance to " + QString::number(lastResistanceBeforeDisconnection) + ". It was the last value before the disconnection.";
        forceResistance(lastResistanceBeforeDisconnection);
        lastResistanceBeforeDisconnection = -1;
    }
}

void echelonrower::stateChanged(QLowEnergyService::ServiceState state)
{
    QBluetoothUuid _gattWriteCharacteristicId((QString)"0bf669f2-45f2-11e7-9598-0800200c9a66");
    QBluetoothUuid _gattNotify1CharacteristicId((QString)"0bf669f3-45f2-11e7-9598-0800200c9a66");
    QBluetoothUuid _gattNotify2CharacteristicId((QString)"0bf669f4-45f2-11e7-9598-0800200c9a66");

    QMetaEnum metaEnum = QMetaEnum::fromType<QLowEnergyService::ServiceState>();
    qDebug() << "BTLE stateChanged " + QString::fromLocal8Bit(metaEnum.valueToKey(state));

    if(state == QLowEnergyService::ServiceDiscovered)
    {
        //qDebug() << gattCommunicationChannelService->characteristics();

        gattWriteCharacteristic = gattCommunicationChannelService->characteristic(_gattWriteCharacteristicId);
        gattNotify1Characteristic = gattCommunicationChannelService->characteristic(_gattNotify1CharacteristicId);
        gattNotify2Characteristic = gattCommunicationChannelService->characteristic(_gattNotify2CharacteristicId);
        Q_ASSERT(gattWriteCharacteristic.isValid());
        Q_ASSERT(gattNotify1Characteristic.isValid());
        Q_ASSERT(gattNotify2Characteristic.isValid());

        // establish hook into notifications
        connect(gattCommunicationChannelService, SIGNAL(characteristicChanged(QLowEnergyCharacteristic,QByteArray)),
                this, SLOT(characteristicChanged(QLowEnergyCharacteristic,QByteArray)));
        connect(gattCommunicationChannelService, SIGNAL(characteristicWritten(const QLowEnergyCharacteristic, const QByteArray)),
                this, SLOT(characteristicWritten(const QLowEnergyCharacteristic, const QByteArray)));
        connect(gattCommunicationChannelService, SIGNAL(error(QLowEnergyService::ServiceError)),
                this, SLOT(errorService(QLowEnergyService::ServiceError)));
        connect(gattCommunicationChannelService, SIGNAL(descriptorWritten(const QLowEnergyDescriptor, const QByteArray)), this,
                SLOT(descriptorWritten(const QLowEnergyDescriptor, const QByteArray)));

        // ******************************************* virtual bike init *************************************
        if(!firstStateChanged && !virtualBike
        #ifdef Q_OS_IOS
        #ifndef IO_UNDER_QT
                && !h
        #endif
        #endif
        )
        {
            QSettings settings;
            bool virtual_device_enabled = settings.value("virtual_device_enabled", true).toBool();
#ifdef Q_OS_IOS
#ifndef IO_UNDER_QT
            bool cadence = settings.value("bike_cadence_sensor", false).toBool();
            bool ios_peloton_workaround = settings.value("ios_peloton_workaround", true).toBool();
            if(ios_peloton_workaround && cadence)
            {
                qDebug() << "ios_peloton_workaround activated!";
                h = new lockscreen();
                h->virtualbike_ios();
            }
            else
#endif
#endif
                if(virtual_device_enabled)
            {
                qDebug() << "creating virtual bike interface...";
                virtualBike = new virtualbike(this, noWriteResistance, noHeartService, bikeResistanceOffset, bikeResistanceGain);
                //connect(virtualBike,&virtualbike::debug ,this,&echelonrower::debug);
            }
        }
        firstStateChanged = 1;
        // ********************************************************************************************************

        QByteArray descriptor;
        descriptor.append((char)0x01);
        descriptor.append((char)0x00);
        gattCommunicationChannelService->writeDescriptor(gattNotify1Characteristic.descriptor(QBluetoothUuid::ClientCharacteristicConfiguration), descriptor);
        gattCommunicationChannelService->writeDescriptor(gattNotify2Characteristic.descriptor(QBluetoothUuid::ClientCharacteristicConfiguration), descriptor);
    }
}

void echelonrower::descriptorWritten(const QLowEnergyDescriptor &descriptor, const QByteArray &newValue)
{
    qDebug() << "descriptorWritten " + descriptor.name() + " " + newValue.toHex(' ');

    initRequest = true;
    emit connectedAndDiscovered();
}

void echelonrower::characteristicWritten(const QLowEnergyCharacteristic &characteristic, const QByteArray &newValue)
{
    Q_UNUSED(characteristic);
    qDebug() << "characteristicWritten " + newValue.toHex(' ');
}

void echelonrower::serviceScanDone(void)
{
    qDebug() << "serviceScanDone";

    QBluetoothUuid _gattCommunicationChannelServiceId((QString)"0bf669f1-45f2-11e7-9598-0800200c9a66");

    gattCommunicationChannelService = m_control->createServiceObject(_gattCommunicationChannelServiceId);
    connect(gattCommunicationChannelService, SIGNAL(stateChanged(QLowEnergyService::ServiceState)), this, SLOT(stateChanged(QLowEnergyService::ServiceState)));
    gattCommunicationChannelService->discoverDetails();
}

void echelonrower::errorService(QLowEnergyService::ServiceError err)
{
    QMetaEnum metaEnum = QMetaEnum::fromType<QLowEnergyService::ServiceError>();
    qDebug() << "echelonrower::errorService" + QString::fromLocal8Bit(metaEnum.valueToKey(err)) + m_control->errorString();
}

void echelonrower::error(QLowEnergyController::Error err)
{
    QMetaEnum metaEnum = QMetaEnum::fromType<QLowEnergyController::Error>();
    qDebug() << "echelonrower::error" + QString::fromLocal8Bit(metaEnum.valueToKey(err)) + m_control->errorString();
}

void echelonrower::deviceDiscovered(const QBluetoothDeviceInfo &device)
{
    qDebug() << "Found new device: " + device.name() + " (" + device.address().toString() + ')';
    if(device.name().startsWith("ECH"))
    {
        bluetoothDevice = device;

        m_control = QLowEnergyController::createCentral(bluetoothDevice, this);
        connect(m_control, SIGNAL(serviceDiscovered(const QBluetoothUuid &)),
                this, SLOT(serviceDiscovered(const QBluetoothUuid &)));
        connect(m_control, SIGNAL(discoveryFinished()),
                this, SLOT(serviceScanDone()));
        connect(m_control, SIGNAL(error(QLowEnergyController::Error)),
                this, SLOT(error(QLowEnergyController::Error)));
        connect(m_control, SIGNAL(stateChanged(QLowEnergyController::ControllerState)), this, SLOT(controllerStateChanged(QLowEnergyController::ControllerState)));

        connect(m_control, static_cast<void (QLowEnergyController::*)(QLowEnergyController::Error)>(&QLowEnergyController::error),
                this, [this](QLowEnergyController::Error error) {
            Q_UNUSED(error);
            Q_UNUSED(this);
            qDebug() << "Cannot connect to remote device.";
            emit disconnected();
        });
        connect(m_control, &QLowEnergyController::connected, this, [this]() {
            Q_UNUSED(this);
            qDebug() << "Controller connected. Search services...";
            m_control->discoverServices();
        });
        connect(m_control, &QLowEnergyController::disconnected, this, [this]() {
            Q_UNUSED(this);
            qDebug() << "LowEnergy controller disconnected";
            emit disconnected();
        });

        // Connect
        m_control->connectToDevice();
        return;
    }
}

bool echelonrower::connected()
{
    if(!m_control)
        return false;
    return m_control->state() == QLowEnergyController::DiscoveredState;
}

void* echelonrower::VirtualBike()
{
    return virtualBike;
}

void* echelonrower::VirtualDevice()
{
    return VirtualBike();
}

uint16_t echelonrower::watts()
{
    if(currentCadence().value() == 0) return 0;
    return wattsFromResistance(Resistance.value());
}

uint16_t echelonrower::wattsFromResistance(double resistance)
{
    // https://github.com/cagnulein/qdomyos-zwift/issues/62#issuecomment-736913564
    /*if(currentCadence().value() < 90)
        return (uint16_t)((3.59 * exp(0.0217 * (double)(currentCadence().value()))) * exp(0.095 * (double)(currentResistance().value())) );
    else
        return (uint16_t)((3.59 * exp(0.0217 * (double)(currentCadence().value()))) * exp(0.088 * (double)(currentResistance().value())) );*/

    const double Epsilon = 4.94065645841247E-324;
    const int wattTableFirstDimension = 33;
    const int wattTableSecondDimension = 12;

    double wattTable[wattTableFirstDimension][wattTableSecondDimension] = {
        {Epsilon, Epsilon, 30.0, 62.0, 88.0, 108.0, 119.0, 128.0, 141.0, 153.0, 162.0, 174.0},
        {Epsilon, Epsilon, 30.0, 62.0, 88.0, 108.0, 119.0, 128.0, 141.0, 153.0, 162.0, 174.0},
        {Epsilon, Epsilon, 31.0, 64.0, 90.0, 113.0, 123.0, 133.0, 144.0, 155.0, 164.0, 176.0},
        {Epsilon, Epsilon, 31.5, 65.0, 93.0, 117.0, 129.0, 137.0, 148.0, 158.0, 167.0, 180.0},
        {Epsilon, Epsilon, 32.0, 66.0, 96.0, 122.0, 134.0, 143.0, 153.0, 161.0, 169.0, 182.0},
        {Epsilon, Epsilon, 32.5, 67.0, 98.0, 127.0, 138.0, 151.0, 161.0, 168.0, 170.0, 184.0},
        {Epsilon, Epsilon, 33.0, 68.0, 103.0, 133.0, 145.0, 157.0, 167.0, 172.0, 174.0, 188.0},
        {Epsilon, Epsilon, 33.5, 69.0, 105.0, 136.0, 151.0, 160.0, 174.0, 179.0, 180.0, 193.0},
        {Epsilon, Epsilon, 34.0, 70.0, 107.0, 140.0, 159.0, 164.0, 177.0, 184.0, 186.0, 198.0},
        {Epsilon, Epsilon, 34.5, 71.0, 110.0, 144.0, 164.0, 168.0, 182.0, 188.0, 190.0, 205.0},
        {Epsilon, Epsilon, 35.0, 72.0, 113.0, 148.0, 169.0, 178.0, 194.0, 202.0, 209.0, 220.0},
        {Epsilon, Epsilon, 35.5, 73.5, 117.0, 155.0, 174.0, 185.0, 199.0, 209.0, 217.0, 227.0},
        {Epsilon, Epsilon, 36.0, 75.0, 120.0, 158.0, 179.0, 188.0, 205.0, 217.0, 225.0, 234.0},
        {Epsilon, Epsilon, 37.0, 76.5, 122.0, 162.0, 186.0, 202.0, 216.0, 225.0, 231.0, 243.0},
        {Epsilon, Epsilon, 38.0, 77.0, 124.0, 165.0, 189.0, 217.0, 224.0, 232.0, 240.0, 254.0},
        {Epsilon, Epsilon, 38.5, 78.0, 128.0, 168.0, 201.0, 221.0, 231.0, 243.0, 249.0, 262.0},
        {Epsilon, Epsilon, 39.5, 79.0, 132.0, 175.0, 208.0, 228.0, 239.0, 251.0, 261.0, 272.0},
        {Epsilon, Epsilon, 40.5, 80.5, 138.0, 180.0, 219.0, 234.0, 246.0, 259.0, 271.0, 281.0},
        {Epsilon, Epsilon, 41.0, 82.0, 145.0, 187.0, 229.0, 246.0, 252.0, 264.0, 278.0, 291.0},
        {Epsilon, Epsilon, 41.5, 85.0, 148.0, 202.0, 239.0, 253.0, 261.0, 270.0, 284.0, 299.0},
        {Epsilon, Epsilon, 42.0, 87.0, 153.0, 205.0, 249.0, 259.0, 266.0, 278.0, 291.0, 310.0},
        {Epsilon, Epsilon, 47.0, 92.0, 160.0, 208.0, 255.0, 266.0, 272.0, 283.0, 296.0, 315.0},
        {Epsilon, Epsilon, 48.0, 95.0, 168.0, 215.0, 269.0, 276.0, 282.0, 298.0, 307.0, 325.0},
        {Epsilon, Epsilon, 51.0, 100.0, 173.0, 218.0, 279.0, 283.0, 290.0, 305.0, 320.0, 341.0},
        {Epsilon, Epsilon, 56.0, 102.0, 177.0, 228.0, 289.0, 294.0, 301.0, 317.0, 330.0, 351.0},
        {Epsilon, Epsilon, 59.0, 108.0, 184.0, 236.0, 298.0, 303.0, 312.0, 323.0, 337.0, 367.0},
        {Epsilon, Epsilon, 61.0, 115.0, 188.0, 238.0, 309.0, 317.0, 325.0, 339.0, 353.0, 380.0},
        {Epsilon, Epsilon, 63.0, 125.0, 193.0, 248.0, 319.0, 329.0, 338.0, 347.0, 364.0, 389.0},
        {Epsilon, Epsilon, 70.0, 132.0, 196.0, 256.0, 324.0, 340.0, 351.0, 375.0, 389.0, 408.0},
        {Epsilon, Epsilon, 75.0, 136.0, 208.0, 258.0, 329.0, 350.0, 360.0, 382.0, 402.0, 421.0},
        {Epsilon, Epsilon, 82.0, 142.0, 213.0, 262.0, 335.0, 357.0, 367.0, 396.0, 414.0, 431.0},
        {Epsilon, Epsilon, 86.0, 152.0, 216.0, 266.0, 339.0, 362.0, 372.0, 415.0, 430.0, 445.0},
        {Epsilon, Epsilon, 90.0, 158.0, 223.0, 268.0, 344.0, 368.0, 399.0, 430.0, 444.0, 460.0}};

    int level = resistance;
    if (level < 0) {
        level = 0;
    }
    if (level >= wattTableFirstDimension) {
        level = wattTableFirstDimension - 1;
    }
    double* watts_of_level = wattTable[level];
    int watt_setp = (Cadence.value() / 5.0);
    if (watt_setp >= 11) {
        return (((double) Cadence.value()) / 55.0) * watts_of_level[wattTableSecondDimension - 1];
    }
    double watt_base = watts_of_level[watt_setp];
    return (((watts_of_level[watt_setp + 1] - watt_base) / 5.0) * ((double) (((int)(Cadence.value())) % 5))) + watt_base;
}

void echelonrower::controllerStateChanged(QLowEnergyController::ControllerState state)
{
    qDebug() << "controllerStateChanged" << state;
    if(state == QLowEnergyController::UnconnectedState && m_control)
    {
        lastResistanceBeforeDisconnection = Resistance.value();
        qDebug() << "trying to connect back again...";
        initDone = false;
        m_control->connectToDevice();
    }
}
