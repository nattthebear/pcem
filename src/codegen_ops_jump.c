#include "ibm.h"

#include "x86.h"
#include "386_common.h"
#include "codegen.h"
#include "codegen_ir.h"
#include "codegen_ops.h"
#include "codegen_ops_helpers.h"
#include "codegen_ops_mov.h"

uint32_t ropJMP_r8(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
        uint32_t offset = (int32_t)(int8_t)fastreadb(cs + op_pc);

        uop_MOV_IMM(ir, IREG_pc, op_pc+1+offset);
        return -1;
}
uint32_t ropJMP_r16(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
        uint32_t offset = (int32_t)(int16_t)fastreadw(cs + op_pc);

        uop_MOV_IMM(ir, IREG_pc, op_pc+2+offset);
        return -1;
}
uint32_t ropJMP_r32(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
        uint32_t offset = fastreadl(cs + op_pc);

        uop_MOV_IMM(ir, IREG_pc, op_pc+4+offset);
        return -1;
}

uint32_t ropCALL_r16(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
        uint32_t offset = (int32_t)(int16_t)fastreadw(cs + op_pc);
        uint16_t ret_addr = op_pc + 2;
        uint16_t dest_addr = ret_addr + offset;
        int sp_reg;

        uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
        sp_reg = LOAD_SP_WITH_OFFSET(ir, -2);
        uop_MEM_STORE_IMM_16(ir, IREG_SS_base, sp_reg, ret_addr);
        SUB_SP(ir, 2);
        uop_MOV_IMM(ir, IREG_pc, dest_addr);

        return -1;
}
uint32_t ropCALL_r32(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
        uint32_t offset = fastreadl(cs + op_pc);
        uint32_t ret_addr = op_pc + 4;
        uint32_t dest_addr = ret_addr + offset;
        int sp_reg;
        
        uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
        sp_reg = LOAD_SP_WITH_OFFSET(ir, -4);
        uop_MEM_STORE_IMM_32(ir, IREG_SS_base, sp_reg, ret_addr);
        SUB_SP(ir, 4);
        uop_MOV_IMM(ir, IREG_pc, dest_addr);
        
        return -1;
}

uint32_t ropRET_16(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
        uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
        
        if (stack32)
                uop_MEM_LOAD_REG(ir, IREG_temp0_W, IREG_SS_base, IREG_ESP);
        else
        {
                uop_MOVZX(ir, IREG_eaaddr, IREG_SP);
                uop_MEM_LOAD_REG(ir, IREG_temp0_W, IREG_SS_base, IREG_eaaddr);
        }
        ADD_SP(ir, 2);
        uop_MOVZX(ir, IREG_pc, IREG_temp0_W);

        return -1;
}
uint32_t ropRET_32(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
        uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);
        
        if (stack32)
                uop_MEM_LOAD_REG(ir, IREG_pc, IREG_SS_base, IREG_ESP);
        else
        {
                uop_MOVZX(ir, IREG_eaaddr, IREG_SP);
                uop_MEM_LOAD_REG(ir, IREG_pc, IREG_SS_base, IREG_eaaddr);
        }
        ADD_SP(ir, 4);

        return -1;
}

uint32_t ropRET_imm_16(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
        uint16_t offset = fastreadw(cs + op_pc);
        
        uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);

        if (stack32)
                uop_MEM_LOAD_REG(ir, IREG_temp0_W, IREG_SS_base, IREG_ESP);
        else
        {
                uop_MOVZX(ir, IREG_eaaddr, IREG_SP);
                uop_MEM_LOAD_REG(ir, IREG_temp0_W, IREG_SS_base, IREG_eaaddr);
        }
        ADD_SP(ir, 2+offset);
        uop_MOVZX(ir, IREG_pc, IREG_temp0_W);

        return -1;
}
uint32_t ropRET_imm_32(codeblock_t *block, ir_data_t *ir, uint8_t opcode, uint32_t fetchdat, uint32_t op_32, uint32_t op_pc)
{
        uint16_t offset = fastreadw(cs + op_pc);

        uop_MOV_IMM(ir, IREG_oldpc, cpu_state.oldpc);

        if (stack32)
                uop_MEM_LOAD_REG(ir, IREG_pc, IREG_SS_base, IREG_ESP);
        else
        {
                uop_MOVZX(ir, IREG_eaaddr, IREG_SP);
                uop_MEM_LOAD_REG(ir, IREG_pc, IREG_SS_base, IREG_eaaddr);
        }
        ADD_SP(ir, 4+offset);

        return -1;
}