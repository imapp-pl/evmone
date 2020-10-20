// evmone: Fast Ethereum Virtual Machine implementation
// Copyright 2020 The evmone Authors.
// SPDX-License-Identifier: Apache-2.0

#include "baseline.hpp"
#include "execution_state.hpp"
#include "instructions.hpp"
#include <evmc/instructions.h>
#include <cassert>
#include <memory>

namespace evmone
{
namespace
{
using JumpdestMap = std::vector<bool>;

JumpdestMap build_jumpdest_map(const uint8_t* code, size_t code_size)
{
    JumpdestMap m(code_size);
    for (size_t i = 0; i < code_size; ++i)
    {
        const auto op = code[i];
        if (op == OP_JUMPDEST)
            m[i] = true;
        else if (op >= OP_PUSH1 && op <= OP_PUSH32)
            i += static_cast<size_t>(op - OP_PUSH1 + 1);
    }
    return m;
}

const uint8_t* op_jump(ExecutionState& state, const JumpdestMap& jumpdest_map) noexcept
{
    const auto dst = state.stack.pop();
    if (dst >= jumpdest_map.size() || !jumpdest_map[static_cast<size_t>(dst)])
    {
        state.status = EVMC_BAD_JUMP_DESTINATION;
        return &*state.code.end();
    }

    return &state.code[static_cast<size_t>(dst)];
}

template <size_t Len>
inline const uint8_t* load_push(
    ExecutionState& state, const uint8_t* code, const uint8_t* code_end) noexcept
{
    // TODO: Also last full push can be ignored.
    if (code + Len > code_end)  // Trimmed push data can be ignored.
        return code_end;

    uint8_t buffer[Len];
    std::memcpy(buffer, code, Len);
    state.stack.push(intx::be::load<intx::uint256>(buffer));
    return code + Len;
}

template <evmc_status_code StatusCode>
inline void op_return(ExecutionState& state) noexcept
{
    const auto offset = state.stack[0];
    const auto size = state.stack[1];

    if (!check_memory(state, offset, size))
    {
        state.status = EVMC_OUT_OF_GAS;
        return;
    }

    state.output_size = static_cast<size_t>(size);
    if (state.output_size != 0)
        state.output_offset = static_cast<size_t>(offset);
    state.status = StatusCode;
}

inline evmc_status_code check_requirements(const char* const* instruction_names,
    const evmc_instruction_metrics* instruction_metrics, ExecutionState& state, uint8_t op) noexcept
{
    const auto metrics = instruction_metrics[op];

    if (instruction_names[op] == nullptr)
        return EVMC_UNDEFINED_INSTRUCTION;

    if ((state.gas_left -= metrics.gas_cost) < 0)
        return EVMC_OUT_OF_GAS;

    const auto stack_size = state.stack.size();
    if (stack_size < metrics.stack_height_required)
        return EVMC_STACK_UNDERFLOW;
    if (stack_size + metrics.stack_height_change > evm_stack::limit)
        return EVMC_STACK_OVERFLOW;

    return EVMC_SUCCESS;
}
}  // namespace

evmc_result baseline_execute(evmc_vm* /*vm*/, const evmc_host_interface* host,
    evmc_host_context* ctx, evmc_revision rev, const evmc_message* msg, const uint8_t* code,
    size_t code_size) noexcept
{
    const auto instruction_names = evmc_get_instruction_names_table(rev);
    const auto instruction_metrics = evmc_get_instruction_metrics_table(rev);
    const auto jumpdest_map = build_jumpdest_map(code, code_size);

    auto state = std::make_unique<ExecutionState>(*msg, rev, *host, ctx, code, code_size);

    const auto code_end = code + code_size;
    auto* pc = code;
    while (pc != code_end)
    {
        const auto op = *pc;

        const auto status = check_requirements(instruction_names, instruction_metrics, *state, op);
        if (status != EVMC_SUCCESS)
        {
            state->status = status;
            goto exit;
        }

        switch (op)
        {
        case OP_STOP:
            goto exit;
        case OP_ADD:
            add(state->stack);
            break;
        case OP_MUL:
            mul(state->stack);
            break;
        case OP_SUB:
            sub(state->stack);
            break;
        case OP_DIV:
            div(state->stack);
            break;
        case OP_SDIV:
            sdiv(state->stack);
            break;
        case OP_MOD:
            mod(state->stack);
            break;
        case OP_SMOD:
            smod(state->stack);
            break;
        case OP_ADDMOD:
            addmod(state->stack);
            break;
        case OP_MULMOD:
            mulmod(state->stack);
            break;
        case OP_EXP:
        {
            const auto status_code = exp(*state);
            if (status_code != EVMC_SUCCESS)
            {
                state->status = status_code;
                goto exit;
            }
            break;
        }
        case OP_SIGNEXTEND:
            signextend(state->stack);
            break;

        case OP_LT:
            lt(state->stack);
            break;
        case OP_GT:
            gt(state->stack);
            break;
        case OP_SLT:
            slt(state->stack);
            break;
        case OP_SGT:
            sgt(state->stack);
            break;
        case OP_EQ:
            eq(state->stack);
            break;
        case OP_ISZERO:
            iszero(state->stack);
            break;
        case OP_AND:
            and_(state->stack);
            break;
        case OP_OR:
            or_(state->stack);
            break;
        case OP_XOR:
            xor_(state->stack);
            break;
        case OP_NOT:
            not_(state->stack);
            break;
        case OP_BYTE:
            byte(state->stack);
            break;
        case OP_SHL:
            shl(state->stack);
            break;
        case OP_SHR:
            shr(state->stack);
            break;
        case OP_SAR:
            sar(state->stack);
            break;

        case OP_SHA3:
        {
            const auto status_code = sha3(*state);
            if (status_code != EVMC_SUCCESS)
            {
                state->status = status_code;
                goto exit;
            }
            break;
        }

        case OP_ADDRESS:
            address(*state);
            break;
        case OP_BALANCE:
            balance(*state);
            break;
        case OP_ORIGIN:
            origin(*state);
            break;
        case OP_CALLER:
            caller(*state);
            break;
        case OP_CALLVALUE:
            callvalue(*state);
            break;
        case OP_CALLDATALOAD:
            calldataload(*state);
            break;
        case OP_CALLDATASIZE:
            calldatasize(*state);
            break;
        case OP_CALLDATACOPY:
        {
            const auto status_code = calldatacopy(*state);
            if (status_code != EVMC_SUCCESS)
            {
                state->status = status_code;
                goto exit;
            }
            break;
        }
        case OP_CODESIZE:
            state->stack.push(code_size);
            break;
        case OP_CODECOPY:
        {
            const auto status_code = codecopy(*state);
            if (status_code != EVMC_SUCCESS)
            {
                state->status = status_code;
                goto exit;
            }
            break;
        }
        case OP_GASPRICE:
            gasprice(*state);
            break;
        case OP_EXTCODESIZE:
            extcodesize(*state);
            break;
        case OP_EXTCODECOPY:
        {
            const auto status_code = extcodecopy(*state);
            if (status_code != EVMC_SUCCESS)
            {
                state->status = status_code;
                goto exit;
            }
            break;
        }
        case OP_RETURNDATASIZE:
            returndatasize(*state);
            break;
        case OP_RETURNDATACOPY:
        {
            const auto status_code = returndatacopy(*state);
            if (status_code != EVMC_SUCCESS)
            {
                state->status = status_code;
                goto exit;
            }
            break;
        }
        case OP_EXTCODEHASH:
            extcodehash(*state);
            break;

        case OP_BLOCKHASH:
            blockhash(*state);
            break;
        case OP_COINBASE:
            coinbase(*state);
            break;
        case OP_TIMESTAMP:
            timestamp(*state);
            break;
        case OP_NUMBER:
            number(*state);
            break;
        case OP_DIFFICULTY:
            difficulty(*state);
            break;
        case OP_GASLIMIT:
            gaslimit(*state);
            break;
        case OP_CHAINID:
            chainid(*state);
            break;
        case OP_SELFBALANCE:
            selfbalance(*state);
            break;

        case OP_POP:
            pop(state->stack);
            break;
        case OP_MLOAD:
        {
            const auto status_code = mload(*state);
            if (status_code != EVMC_SUCCESS)
            {
                state->status = status_code;
                goto exit;
            }
            break;
        }
        case OP_MSTORE:
        {
            const auto status_code = mstore(*state);
            if (status_code != EVMC_SUCCESS)
            {
                state->status = status_code;
                goto exit;
            }
            break;
        }
        case OP_MSTORE8:
        {
            const auto status_code = mstore8(*state);
            if (status_code != EVMC_SUCCESS)
            {
                state->status = status_code;
                goto exit;
            }
            break;
        }

        case OP_JUMP:
            pc = op_jump(*state, jumpdest_map);
            continue;
        case OP_JUMPI:
            if (state->stack[1] != 0)
            {
                pc = op_jump(*state, jumpdest_map);
            }
            else
            {
                state->stack.pop();
                ++pc;
            }
            state->stack.pop();
            continue;

        case OP_PC:
            state->stack.push(pc - code);
            break;
        case OP_MSIZE:
            msize(*state);
            break;
        case OP_SLOAD:
            sload(*state);
            break;
        case OP_SSTORE:
        {
            const auto status_code = sstore(*state);
            if (status_code != EVMC_SUCCESS)
            {
                state->status = status_code;
                goto exit;
            }
            break;
        }
        case OP_GAS:
            state->stack.push(state->gas_left);
            break;
        case OP_JUMPDEST:
            break;

        case OP_PUSH1:
            pc = load_push<1>(*state, pc + 1, code_end);
            continue;
        case OP_PUSH2:
            pc = load_push<2>(*state, pc + 1, code_end);
            continue;
        case OP_PUSH3:
            pc = load_push<3>(*state, pc + 1, code_end);
            continue;
        case OP_PUSH4:
            pc = load_push<4>(*state, pc + 1, code_end);
            continue;
        case OP_PUSH5:
            pc = load_push<5>(*state, pc + 1, code_end);
            continue;
        case OP_PUSH6:
            pc = load_push<6>(*state, pc + 1, code_end);
            continue;
        case OP_PUSH7:
            pc = load_push<7>(*state, pc + 1, code_end);
            continue;
        case OP_PUSH8:
            pc = load_push<8>(*state, pc + 1, code_end);
            continue;
        case OP_PUSH9:
            pc = load_push<9>(*state, pc + 1, code_end);
            continue;
        case OP_PUSH10:
            pc = load_push<10>(*state, pc + 1, code_end);
            continue;
        case OP_PUSH11:
            pc = load_push<11>(*state, pc + 1, code_end);
            continue;
        case OP_PUSH12:
            pc = load_push<12>(*state, pc + 1, code_end);
            continue;
        case OP_PUSH13:
            pc = load_push<13>(*state, pc + 1, code_end);
            continue;
        case OP_PUSH14:
            pc = load_push<14>(*state, pc + 1, code_end);
            continue;
        case OP_PUSH15:
            pc = load_push<15>(*state, pc + 1, code_end);
            continue;
        case OP_PUSH16:
            pc = load_push<16>(*state, pc + 1, code_end);
            continue;
        case OP_PUSH17:
            pc = load_push<17>(*state, pc + 1, code_end);
            continue;
        case OP_PUSH18:
            pc = load_push<18>(*state, pc + 1, code_end);
            continue;
        case OP_PUSH19:
            pc = load_push<19>(*state, pc + 1, code_end);
            continue;
        case OP_PUSH20:
            pc = load_push<20>(*state, pc + 1, code_end);
            continue;
        case OP_PUSH21:
            pc = load_push<21>(*state, pc + 1, code_end);
            continue;
        case OP_PUSH22:
            pc = load_push<22>(*state, pc + 1, code_end);
            continue;
        case OP_PUSH23:
            pc = load_push<23>(*state, pc + 1, code_end);
            continue;
        case OP_PUSH24:
            pc = load_push<24>(*state, pc + 1, code_end);
            continue;
        case OP_PUSH25:
            pc = load_push<25>(*state, pc + 1, code_end);
            continue;
        case OP_PUSH26:
            pc = load_push<26>(*state, pc + 1, code_end);
            continue;
        case OP_PUSH27:
            pc = load_push<27>(*state, pc + 1, code_end);
            continue;
        case OP_PUSH28:
            pc = load_push<28>(*state, pc + 1, code_end);
            continue;
        case OP_PUSH29:
            pc = load_push<29>(*state, pc + 1, code_end);
            continue;
        case OP_PUSH30:
            pc = load_push<30>(*state, pc + 1, code_end);
            continue;
        case OP_PUSH31:
            pc = load_push<31>(*state, pc + 1, code_end);
            continue;
        case OP_PUSH32:
            pc = load_push<32>(*state, pc + 1, code_end);
            continue;

        case OP_DUP1:
            dup<OP_DUP1>(state->stack);
            break;
        case OP_DUP2:
            dup<OP_DUP2>(state->stack);
            break;
        case OP_DUP3:
            dup<OP_DUP3>(state->stack);
            break;
        case OP_DUP4:
            dup<OP_DUP4>(state->stack);
            break;
        case OP_DUP5:
            dup<OP_DUP5>(state->stack);
            break;
        case OP_DUP6:
            dup<OP_DUP6>(state->stack);
            break;
        case OP_DUP7:
            dup<OP_DUP7>(state->stack);
            break;
        case OP_DUP8:
            dup<OP_DUP8>(state->stack);
            break;
        case OP_DUP9:
            dup<OP_DUP9>(state->stack);
            break;
        case OP_DUP10:
            dup<OP_DUP10>(state->stack);
            break;
        case OP_DUP11:
            dup<OP_DUP11>(state->stack);
            break;
        case OP_DUP12:
            dup<OP_DUP12>(state->stack);
            break;
        case OP_DUP13:
            dup<OP_DUP13>(state->stack);
            break;
        case OP_DUP14:
            dup<OP_DUP14>(state->stack);
            break;
        case OP_DUP15:
            dup<OP_DUP15>(state->stack);
            break;
        case OP_DUP16:
            dup<OP_DUP16>(state->stack);
            break;

        case OP_SWAP1:
            swap<OP_SWAP1>(state->stack);
            break;
        case OP_SWAP2:
            swap<OP_SWAP2>(state->stack);
            break;
        case OP_SWAP3:
            swap<OP_SWAP3>(state->stack);
            break;
        case OP_SWAP4:
            swap<OP_SWAP4>(state->stack);
            break;
        case OP_SWAP5:
            swap<OP_SWAP5>(state->stack);
            break;
        case OP_SWAP6:
            swap<OP_SWAP6>(state->stack);
            break;
        case OP_SWAP7:
            swap<OP_SWAP7>(state->stack);
            break;
        case OP_SWAP8:
            swap<OP_SWAP8>(state->stack);
            break;
        case OP_SWAP9:
            swap<OP_SWAP9>(state->stack);
            break;
        case OP_SWAP10:
            swap<OP_SWAP10>(state->stack);
            break;
        case OP_SWAP11:
            swap<OP_SWAP11>(state->stack);
            break;
        case OP_SWAP12:
            swap<OP_SWAP12>(state->stack);
            break;
        case OP_SWAP13:
            swap<OP_SWAP13>(state->stack);
            break;
        case OP_SWAP14:
            swap<OP_SWAP14>(state->stack);
            break;
        case OP_SWAP15:
            swap<OP_SWAP15>(state->stack);
            break;
        case OP_SWAP16:
            swap<OP_SWAP16>(state->stack);
            break;

        case OP_LOG0:
        {
            const auto status_code = log(*state, 0);
            if (status_code != EVMC_SUCCESS)
            {
                state->status = status_code;
                goto exit;
            }
            break;
        }
        case OP_LOG1:
        {
            const auto status_code = log(*state, 1);
            if (status_code != EVMC_SUCCESS)
            {
                state->status = status_code;
                goto exit;
            }
            break;
        }
        case OP_LOG2:
        {
            const auto status_code = log(*state, 2);
            if (status_code != EVMC_SUCCESS)
            {
                state->status = status_code;
                goto exit;
            }
            break;
        }
        case OP_LOG3:
        {
            const auto status_code = log(*state, 3);
            if (status_code != EVMC_SUCCESS)
            {
                state->status = status_code;
                goto exit;
            }
            break;
        }
        case OP_LOG4:
        {
            const auto status_code = log(*state, 4);
            if (status_code != EVMC_SUCCESS)
            {
                state->status = status_code;
                goto exit;
            }
            break;
        }

        case OP_CREATE:
        {
            const auto status_code = create<EVMC_CREATE>(*state);
            if (status_code != EVMC_SUCCESS)
            {
                state->status = status_code;
                goto exit;
            }
            break;
        }
        case OP_CALL:
        {
            const auto status_code = call<EVMC_CALL>(*state);
            if (status_code != EVMC_SUCCESS)
            {
                state->status = status_code;
                goto exit;
            }
            break;
        }
        case OP_CALLCODE:
        {
            const auto status_code = call<EVMC_CALLCODE>(*state);
            if (status_code != EVMC_SUCCESS)
            {
                state->status = status_code;
                goto exit;
            }
            break;
        }
        case OP_RETURN:
            op_return<EVMC_SUCCESS>(*state);
            goto exit;
        case OP_DELEGATECALL:
        {
            const auto status_code = call<EVMC_DELEGATECALL>(*state);
            if (status_code != EVMC_SUCCESS)
            {
                state->status = status_code;
                goto exit;
            }
            break;
        }
        case OP_STATICCALL:
        {
            const auto status_code = call<EVMC_CALL, true>(*state);
            if (status_code != EVMC_SUCCESS)
            {
                state->status = status_code;
                goto exit;
            }
            break;
        }
        case OP_CREATE2:
        {
            const auto status_code = create<EVMC_CREATE2>(*state);
            if (status_code != EVMC_SUCCESS)
            {
                state->status = status_code;
                goto exit;
            }
            break;
        }
        case OP_REVERT:
            op_return<EVMC_REVERT>(*state);
            goto exit;
        case OP_INVALID:
            state->status = EVMC_INVALID_INSTRUCTION;
            goto exit;
        case OP_SELFDESTRUCT:
            state->status = selfdestruct(*state);
            goto exit;
        default:
            assert(false);
            INTX_UNREACHABLE;
        }

        ++pc;
    }

exit:
    const auto gas_left =
        (state->status == EVMC_SUCCESS || state->status == EVMC_REVERT) ? state->gas_left : 0;

    return evmc::make_result(
        state->status, gas_left, &state->memory[state->output_offset], state->output_size);
}
}  // namespace evmone