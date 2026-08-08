#include "net/loot_transaction.hpp"
#include <array>
#include <iostream>
int main(){using namespace kraken::net;std::array<Byte,kLootRequestWireSize>rq{};LootRequest a{2,9,11,3},b{};if(encode_loot_request(a,rq)!=LootCodecError::None||decode_loot_request(rq,b)!=LootCodecError::None||b.transaction_id!=11)return 1;a.amount=0;if(encode_loot_request(a,rq)!=LootCodecError::InvalidAmount)return 2;std::array<Byte,kLootResultWireSize>rs{};LootResult r{11,9,44,3,7,LootResultCode::Granted},s{};if(encode_loot_result(r,rs)!=LootCodecError::None||decode_loot_result(rs,s)!=LootCodecError::None||s.remaining_amount!=7)return 3;rs[6]=static_cast<Byte>(99);if(decode_loot_result(rs,s)!=LootCodecError::InvalidResultCode)return 4;std::cout<<"loot transaction tests passed\n";}
