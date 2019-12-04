// Copyright 2019 Josh Pieper, jjp@pobox.com.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "fw/fdcan.h"

#include "PeripheralPins.h"

extern const PinMap PinMap_CAN_TD[];
extern const PinMap PinMap_CAN_RD[];

namespace fw {
namespace {
constexpr uint32_t RoundUpDlc(size_t size) {
  if (size == 0) { return FDCAN_DLC_BYTES_0; }
  if (size == 1) { return FDCAN_DLC_BYTES_1; }
  if (size == 2) { return FDCAN_DLC_BYTES_2; }
  if (size == 3) { return FDCAN_DLC_BYTES_3; }
  if (size == 4) { return FDCAN_DLC_BYTES_4; }
  if (size == 5) { return FDCAN_DLC_BYTES_5; }
  if (size == 6) { return FDCAN_DLC_BYTES_6; }
  if (size == 7) { return FDCAN_DLC_BYTES_7; }
  if (size == 8) { return FDCAN_DLC_BYTES_8; }
  if (size <= 12) { return FDCAN_DLC_BYTES_12; }
  if (size <= 16) { return FDCAN_DLC_BYTES_16; }
  if (size <= 20) { return FDCAN_DLC_BYTES_20; }
  if (size <= 24) { return FDCAN_DLC_BYTES_24; }
  if (size <= 32) { return FDCAN_DLC_BYTES_32; }
  if (size <= 48) { return FDCAN_DLC_BYTES_48; }
  if (size <= 64) { return FDCAN_DLC_BYTES_64; }
  return 0;
}
}

FDCan::FDCan(const Options& options) {
  __HAL_RCC_FDCAN_CLK_ENABLE();

  {
    const auto can_td = pinmap_peripheral(options.td, PinMap_CAN_TD);
    const auto can_rd = pinmap_peripheral(options.rd, PinMap_CAN_RD);
    can_ = reinterpret_cast<FDCAN_GlobalTypeDef*>(
        pinmap_merge(can_td, can_rd));
  }

  pinmap_pinout(options.td, PinMap_CAN_TD);
  pinmap_pinout(options.rd, PinMap_CAN_RD);

  auto& can = hfdcan1_;

  can.Instance = FDCAN1;
  can.Init.ClockDivider = FDCAN_CLOCK_DIV1;
  can.Init.FrameFormat = FDCAN_FRAME_FD_BRS;
  can.Init.Mode = FDCAN_MODE_NORMAL;
  can.Init.AutoRetransmission = DISABLE;
  can.Init.TransmitPause = ENABLE;
  can.Init.ProtocolException = DISABLE;
  can.Init.NominalPrescaler = 2;
  can.Init.NominalSyncJumpWidth = 16;
  can.Init.NominalTimeSeg1 = 63;
  can.Init.NominalTimeSeg2 = 16;
  can.Init.DataPrescaler = 4;
  can.Init.DataSyncJumpWidth = 2;
  can.Init.DataTimeSeg1 = 3;
  can.Init.DataTimeSeg2 = 2;
  can.Init.StdFiltersNbr = 1;
  can.Init.ExtFiltersNbr = 0;
  can.Init.TxFifoQueueMode = FDCAN_TX_FIFO_OPERATION;
  if (HAL_FDCAN_Init(&can) != HAL_OK) {
    mbed_die();
  }

  /* Configure Rx filter */
  {
    FDCAN_FilterTypeDef sFilterConfig;
    sFilterConfig.IdType = FDCAN_STANDARD_ID;
    sFilterConfig.FilterIndex = 0;
    sFilterConfig.FilterType = FDCAN_FILTER_MASK;
    sFilterConfig.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
    sFilterConfig.FilterID1 = 0x321;
    sFilterConfig.FilterID2 = 0x7FF;
    if (HAL_FDCAN_ConfigFilter(&can, &sFilterConfig) != HAL_OK)
    {
      mbed_die();
    }
  }


  /* Configure global filter:
     Filter all remote frames with STD and EXT ID
     Reject non matching frames with STD ID and EXT ID */
  if (HAL_FDCAN_ConfigGlobalFilter(
          &can, FDCAN_REJECT, FDCAN_REJECT,
          FDCAN_FILTER_REMOTE, FDCAN_FILTER_REMOTE) != HAL_OK)
  {
    mbed_die();
  }

  if (HAL_FDCAN_Start(&can) != HAL_OK) {
    mbed_die();
  }

  if (HAL_FDCAN_ActivateNotification(
          &can, FDCAN_IT_RX_FIFO0_NEW_MESSAGE, 0) != HAL_OK) {
    mbed_die();
  }
}

void FDCan::Send(uint16_t dest_id,
                 std::string_view data) {

  // Abort anything we have started that hasn't finished.
  if (last_tx_request_) {
    HAL_FDCAN_AbortTxRequest(&hfdcan1_, last_tx_request_);
  }

  FDCAN_TxHeaderTypeDef tx_header;
  tx_header.Identifier = dest_id;
  tx_header.IdType = FDCAN_STANDARD_ID;
  tx_header.TxFrameType = FDCAN_DATA_FRAME;
  tx_header.DataLength = RoundUpDlc(data.size());
  tx_header.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
  tx_header.BitRateSwitch = FDCAN_BRS_ON;
  tx_header.FDFormat = FDCAN_FD_CAN;
  tx_header.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
  tx_header.MessageMarker = 0;

  if (HAL_FDCAN_AddMessageToTxFifoQ(
          &hfdcan1_, &tx_header,
          const_cast<uint8_t*>(
              reinterpret_cast<const uint8_t*>(data.data()))) != HAL_OK) {
    mbed_die();
  }
  last_tx_request_ = HAL_FDCAN_GetLatestTxFifoQRequestBuffer(&hfdcan1_);
}

bool FDCan::Poll(FDCAN_RxHeaderTypeDef* header,
                 mjlib::base::string_span data) {
  if (HAL_FDCAN_GetRxMessage(
          &hfdcan1_, FDCAN_RX_FIFO0, header,
          reinterpret_cast<uint8_t*>(data.data())) != HAL_OK) {
    return false;
  }

  return true;
}

}