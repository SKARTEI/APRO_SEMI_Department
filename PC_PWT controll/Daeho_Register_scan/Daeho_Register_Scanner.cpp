#include "ModbusRTU.h"

#include <iostream>
#include <fstream>
#include <vector>
#include <string>

using namespace std;

const unsigned char SLAVE_ID = 1;
const int BLOCK_SIZE = 20;

int main()
{
    cout << "==================================" << endl;
    cout << " CX9230 Register Scanner" << endl;
    cout << "==================================" << endl;

    string comNumber;

    cout << "COM Port Number : ";
    cin >> comNumber;

    unsigned int fcInput;

    cout << "Function Code (3 or 4) : ";
    cin >> fcInput;

    if (fcInput != 3 && fcInput != 4)
    {
        cout << endl;
        cout << "INVALID FUNCTION CODE" << endl;
        return 0;
    }

    unsigned char functionCode =
        static_cast<unsigned char>(fcInput);

    string portName =
        "\\\\.\\COM" + comNumber;

    ModbusRTU modbus;

    if (!modbus.OpenPort(portName.c_str()))
    {
        cout << endl;
        cout << "COM OPEN FAIL" << endl;
        return 0;
    }

    cout << endl;
    cout << "COM OPEN SUCCESS" << endl;

    string registerType;
    string csvName;

    if (functionCode == 0x03)
    {
        registerType = "Holding Register";
        csvName = "HoldingScan.csv";
    }
    else
    {
        registerType = "Input Register";
        csvName = "InputScan.csv";
    }

    ofstream csv(csvName);

    if (!csv.is_open())
    {
        cout << "CSV OPEN FAIL" << endl;
        return 0;
    }

    csv << "Address,Value\n";

    cout << endl;
    cout << "==================================" << endl;
    cout << registerType << " Scan Start" << endl;
    cout << "Address Range : 1 ~ 300" << endl;
    cout << "Block Size    : " << BLOCK_SIZE << endl;
    cout << "==================================" << endl;
    cout << endl;

    int successCount = 0;
    int failBlockCount = 0;

    for (int startAddr = 1;
        startAddr <= 300;
        startAddr += BLOCK_SIZE)
    {
        unsigned short quantity =
            static_cast<unsigned short>(BLOCK_SIZE);

        if (startAddr + quantity - 1 > 300)
        {
            quantity =
                static_cast<unsigned short>(
                    300 - startAddr + 1);
        }

        vector<unsigned short> values;

        bool result =
            modbus.ReadRegisters(
                SLAVE_ID,
                functionCode,
                static_cast<unsigned short>(startAddr),
                quantity,
                values);

        if (!result)
        {
            cout
                << "[FAIL] "
                << startAddr
                << " ~ "
                << (startAddr + quantity - 1)
                << endl;

            failBlockCount++;

            continue;
        }

        cout
            << "[OK] "
            << startAddr
            << " ~ "
            << (startAddr + quantity - 1)
            << endl;

        for (size_t i = 0;
            i < values.size();
            i++)
        {
            int address =
                startAddr + static_cast<int>(i);

            cout
                << address
                << " = "
                << values[i]
                << endl;

            csv
                << address
                << ","
                << values[i]
                << "\n";

            successCount++;
        }
    }

    csv.close();

    cout << endl;
    cout << "==================================" << endl;
    cout << "SCAN COMPLETE" << endl;
    cout << "==================================" << endl;

    cout << "Register Type : "
        << registerType
        << endl;

    cout << "Success Count : "
        << successCount
        << endl;

    cout << "Fail Blocks   : "
        << failBlockCount
        << endl;

    cout << "CSV File      : "
        << csvName
        << endl;

    cout << "==================================" << endl;

    cout << endl;
    cout << "Press ENTER to Exit...";

    cin.ignore();
    cin.get();

    return 0;
}
