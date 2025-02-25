#ifndef VIRTUALTREADMILL_H
#define VIRTUALTREADMILL_H

#include <QObject>

#include <QtBluetooth/qlowenergyadvertisingdata.h>
#include <QtBluetooth/qlowenergyadvertisingparameters.h>
#include <QtBluetooth/qlowenergycharacteristic.h>
#include <QtBluetooth/qlowenergycharacteristicdata.h>
#include <QtBluetooth/qlowenergydescriptordata.h>
#include <QtBluetooth/qlowenergycontroller.h>
#include <QtBluetooth/qlowenergyservice.h>
#include <QtBluetooth/qlowenergyservicedata.h>
#include <QtCore/qbytearray.h>
#ifndef Q_OS_ANDROID
#include <QtCore/qcoreapplication.h>
#else
#include <QtGui/qguiapplication.h>
#endif
#include <QtCore/qlist.h>
#include <QtCore/qloggingcategory.h>
#include <QtCore/qscopedpointer.h>
#include <QtCore/qtimer.h>

#include "treadmill.h"

class virtualtreadmill: public QObject
{
    Q_OBJECT
public:
    virtualtreadmill(bluetoothdevice* t, bool noHeartService);
    bool connected();

private:
    QLowEnergyController* leController = 0;
    QLowEnergyService* service = 0;
    QLowEnergyService* serviceHR = 0;
    QLowEnergyAdvertisingData advertisingData;
    QLowEnergyServiceData serviceData;
    QLowEnergyServiceData serviceDataHR;
    QTimer treadmillTimer;    
    bluetoothdevice* treadMill;

    bool noHeartService = false;

signals:
    void debug(QString string);

private slots:
    void characteristicChanged(const QLowEnergyCharacteristic &characteristic, const QByteArray &newValue);
    void treadmillProvider();
    void reconnect();
};

#endif // VIRTUALTREADMILL_H
