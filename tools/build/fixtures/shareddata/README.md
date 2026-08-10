# DLLSHAREDATA ABI fixtures

`shareddata_abi_contract_test --print-observation` emits the complete top-level
layout observation for the active compiler architecture. It is checked against
`dllsharedata-layout-v1.json` by matching constexpr values in
`DLLSHAREDATA_Abi.h`; the JSON is the reviewable copy of those values. Update
both only as an intentional ABI revision, together with the mapping structure
version and compatibility migration.
