/*
  MSP430 Emulator
  Copyright (C) 2020 Rudolf Geosits (rgeosits@live.esu.edu)

  "MSP430 Emulator" is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  "MSP430 Emulator" is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program. If not, see <https://www.gnu.org/licenses/>.
*/

#include "debugger.h"
#include "io.h"
extern uint8_t* MEMSPACE;

Emulator *local_emu = NULL;

bool exec_cmd (Emulator *emu, char *line, int len)
{
  Cpu *cpu = emu->cpu;
  Debugger *deb = emu->debugger;

  char cmd[100] = {0};
  unsigned int op1 = 0, op2 = 0, op3 = 0;
  int ops;

  char bogus1[100] = {0};
  uint32_t bogus2 = 0, bogus3 = 0;
  deb->error = 0;
  ops = sscanf(line, "%s %u %u", cmd, &op1, &op2);
  //printf("Got %s, %u, %u - ops %d\n", cmd, op1, op2, ops);

  /* RESET / RESTART

     Resets the entire virtual machine to it's starting state.
     Puts the starting address back into Program Counter
   */
  if ( !strncasecmp("reset", cmd, sizeof "reset") ||
       !strncasecmp("restart", cmd, sizeof "restart"))
    {
      // Reset interrupt
      uint16_t resetIntHandlerAddress = ((uint16_t*)MEMSPACE)[0xfffe / 2];
      cpu->pc = resetIntHandlerAddress;

      reset_cpu_stats(emu);
      reset_call_tracer(emu);
      display_registers(emu);
      disassemble(emu, cpu->pc, 1);
    }

  // s [NUM], step NUM instructions forward, defaults to 1 //
  else if ( !strncasecmp("s", cmd, sizeof "s") ||
      !strncasecmp("step", cmd, sizeof "step"))
    {
      int steps = 1; // 1 step by default
      uint32_t i;

      if (ops == 2) {
         steps = (int) op1;
      }

      for (i = 0;i < steps;i++) {
        // Let's handle breakpoints - except the one we are stopped on.
        if (handle_breakpoints(emu) && i > 0)
          break;
        decode(emu, fetch(emu, true), EXECUTE);

        // Handle Peripherals
        handle_bcm(emu);
        handle_timer_a(emu);
        handle_port_1(emu);
        handle_usci(emu);
        if (emu->debugger->error != 0)
          break;
      }

      display_registers(emu);
      disassemble(emu, cpu->pc, 1);
  }

  // Quit program //
  else if ( !strncasecmp("quit", cmd, sizeof "quit") ||
      !strncasecmp("q", cmd, sizeof "q"))
    {
      // This flag stops the main loop in main.c
      deb->quit = true;
    }

  // run the program until a breakpoint is hit //
  else if ( !strncasecmp("run", cmd, sizeof "run") ||
      !strncasecmp("r", cmd, sizeof "r"))
    {
      cpu->running = true;
      deb->debug_mode = false;

      //update_register_display(emu);
    }

  // Display disassembly of N at HEX_ADDR: dis [N] [HEX_ADDR] //
  else if ( !strncasecmp("disas", cmd, sizeof "disas") ||
      !strncasecmp("dis", cmd, sizeof "dis") ||
      !strncasecmp("disassemble", cmd, sizeof "disassemble"))
    {
      uint16_t start_addr = cpu->pc;
      uint32_t num = 10;

      ops = sscanf(line, "%s %u %X", bogus1, &bogus2, &bogus3);

      if (ops == 2) {
        sscanf(line, "%s %u", bogus1, &num);
      }
      else if (ops == 3) {
        sscanf(line, "%s %u %hX", bogus1, &num,
         (uint16_t *)&start_addr);
      }

      disassemble(emu, start_addr, num);
    }

  else if ( !strncasecmp("dump", cmd, sizeof "dump" ))
    {
      char str[100] = {0};
      uint16_t start_addr = cpu->pc;
      uint32_t stride;

      sscanf(line, "%s %s", bogus1, str);

      // Is it a direct address or an adress in a register being spec'd
      if (str[0] >= '0' && str[0] <= '9') {
        sscanf(str, "%X", (unsigned int *) &start_addr);
      }
      else if (str[0] == '%' || str[0] == 'r' || str[0] == 'R')
      {
        uint16_t *p = (uint16_t *)get_reg_ptr(emu, reg_name_to_num(str));
        start_addr = *p;
      }

      stride = BYTE_STRIDE;
      dump_memory(emu, MEMSPACE, 0x0, start_addr, stride);
    }

  // Set REG/LOC VALUE
  else if ( !strncasecmp("set", cmd, sizeof "set") )
    {
      uint16_t value = 0;
      char reg_name_or_addr[50] = {0};
      char *reg_name = NULL;
      char *addr_str = NULL;

      //print_console(emu, "Not yet implemented.\n");

      sscanf(line, "%s %s %X", bogus1, reg_name_or_addr,
       (unsigned int*)&value);

      //printf("Got %s and %X\n", reg_name_or_addr, value);

      // Figure out if the value given is a reg name or addr
      int res = reg_name_to_num(reg_name_or_addr);

      if (res != -1) {  // If its a reg name
        reg_name = reg_name_or_addr;
        printf("In reg part...\n");

        uint16_t *p = (uint16_t*)get_reg_ptr(emu, res);
        *p = value;

        display_registers(emu);
        disassemble(emu, cpu->pc, 1);
      }
      else {
        addr_str = reg_name_or_addr;
        printf("In addr part...\n");

        uint16_t virtual_addr = (uint16_t) strtol(addr_str, NULL, 0);

        uint16_t *p = get_addr_ptr(virtual_addr);
        *p = value;
      }
    }

  // break BREAKPOINT_ADDRESS - set breakpoint //
  else if ( !strncasecmp("break", cmd, sizeof "break") )
    {
      if (deb->num_bps >= MAX_BREAKPOINTS) {
        print_console(emu, "Breakpoints are full.\n");
        return true;
      }

      ops = sscanf(line, "%s %X", bogus1, &bogus2);
      char entry[100] = {0};

      if (ops == 2) {
        sscanf(line, "%s %X", bogus1, (unsigned int *)&deb->bp_addresses[deb->num_bps]);
        sprintf(entry, "\n\t[Breakpoint PC[%d] Set]\n", deb->num_bps + 1);
        print_console(emu, entry);
        ++deb->num_bps;
      }
      else {
        print_console(emu, "error\n");
      }
    }

  // memorybreak BREAKPOINT_ADDRESS - set memory breakpoint //
  else if ( !strncasecmp("memorybreak", cmd, sizeof "memorybreak") )
    {
      if (deb->num_memory_bps >= MAX_BREAKPOINTS) {
        print_console(emu, "Breakpoints are full.\n");
        return true;
      }

      ops = sscanf(line, "%s %X", bogus1, &bogus2);
      char entry[100] = {0};

      if (ops == 2) {
        sscanf(line, "%s %X", bogus1, (unsigned int *)&deb->memory_bp_addresses[deb->num_memory_bps]);
        sprintf(entry, "\n\t[Breakpoint MEM[%d] Set]\n", deb->num_memory_bps + 1);
        print_console(emu, entry);
        ++deb->num_memory_bps;
      }
      else {
        print_console(emu, "error\n");
      }
    }

  // Display all breakpoints //
  else if ( !strncasecmp("bps", cmd, sizeof "bps" ))
    {
      char entry[100] = {0};

      if ((deb->num_bps > 0) || (deb->num_memory_bps > 0)) {

        for (int i = 0; i < deb->num_bps; i++) {
          sprintf(entry, "\tPC[%d] 0x%04X\n", i+1, deb->bp_addresses[i]);
          print_console(emu, entry);
        }

        for (int i = 0; i < deb->num_memory_bps; i++) {
          sprintf(entry, "\tMEM[%d] 0x%04X\n", i+1, deb->memory_bp_addresses[i]);
          print_console(emu, entry);
        }
      }
      else {
        print_console(emu, "You have not set any breakpoints!\n");
      }
    }

  // Display registers //
  else if ( !strncasecmp("regs", cmd, sizeof "regs"))
    {
      display_registers(emu);
      disassemble(emu, cpu->pc, 1);
    }

  // Show instruction trace //
  else if ( !strncasecmp("trace", cmd, sizeof "trace"))
    {
      char trace_mode[sizeof line];
      sscanf(line, "%s %s", bogus1, trace_mode);
      if (strncasecmp("on", trace_mode, sizeof "on") == 0)
        emu->do_trace = true;
      if (strncasecmp("off", trace_mode, sizeof "off") == 0)
        emu->do_trace = false;

      print_console(emu, "Tracing is ");
      print_console(emu, emu->do_trace ? "on\n" : "off\n");
    }

  // Show CPU statistics
  else if (!strncasecmp("stats", cmd, sizeof "stats"))
  {
    display_cpu_stats(emu);
  }

  // help, display a list of debugger cmds //
  else if ( !strncasecmp("help", cmd, sizeof "help") ||
      !strncasecmp("h", cmd, sizeof "h") )
  {
    display_help(emu);
  }

  // End the line loop, next instruction
  else
  {
    print_console(emu, "\t[Invalid command, type \"help\".]\n");
  }

  return true;
}

//##########+++ Dump Memory Function +++##########
void dump_memory ( Emulator *emu, uint8_t *MEM, uint32_t size, uint32_t start_addr, uint8_t stride)
{
  uint32_t i, msp_addr = start_addr;
  MEM += start_addr;
  char str[100] = {0};

  puts("");
  print_console(emu, "\n");

  for (i = 0; i < 32; i += 8) {
    sprintf(str, "0x%04X:\t", msp_addr);
    print_console(emu, str);

    if ( stride == BYTE_STRIDE ) {
      sprintf(str, "0x%02X  0x%02X  0x%02X  0x%02X  "\
        "0x%02X  0x%02X  0x%02X  0x%02X\n",
        *(MEM+0),*(MEM+1),*(MEM+2),*(MEM+3),
        *(MEM+4),*(MEM+5),*(MEM+6),*(MEM+7));

      print_console(emu, str);
    }
    else if ( stride == WORD_STRIDE ) {
      printf("0x%02X%02X  0x%02X%02X  0x%02X%02X  0x%02X%02X\n",
             *(MEM+0),*(MEM+1),*(MEM+2),*(MEM+3),*(MEM+4),
       *(MEM+5),*(MEM+6),*(MEM+7));
    }
    else if ( stride == DWORD_STRIDE ) {
      printf("0x%02X%02X%02X%02X  0x%02X%02X%02X%02X\n",
             *(MEM+0),*(MEM+1),*(MEM+2),*(MEM+3),*(MEM+4),
       *(MEM+5),*(MEM+6),*(MEM+7));
    }

    MEM += 8;        // Increase character by 4
    msp_addr += 8;   // Increase msp_addr by 4
  }

  puts("");
}

void setup_debugger(Emulator *emu)
{
  local_emu = emu;
  Debugger *deb = emu->debugger;

  switch (emu->mode)
  {
    case Emulator_Mode_Web:
      deb->web_interface = true;
      deb->console_interface = false;
      break;

    case Emulator_Mode_Cli:
      deb->web_interface = false;
      deb->console_interface = true;
      break;
  }

  deb->debug_mode = true;
  deb->disassemble_mode = false;
  deb->quit = false;

  deb->web_server_ready = false;
  deb->web_firmware_uploaded = false;


  memset(deb->bp_addresses, 0, sizeof(deb->bp_addresses));
  deb->num_bps = 0;
  deb->num_memory_bps = 0;
}

void handle_sigint(int sig)
{
  if (local_emu == NULL) return;

  local_emu->cpu->running = false;
  local_emu->debugger->debug_mode = true;
}

void register_signal(int sig)
{
  signal(sig, handle_sigint);
}

static void handle_breakpoint_hit(Emulator *emu)
{
  Cpu *cpu = emu->cpu;
  Debugger *deb = emu->debugger;
  cpu->running = false;
  deb->debug_mode = true;

  display_registers(emu);
  disassemble(emu, cpu->pc, 1);
}

bool handle_breakpoints (Emulator *emu)
{
  uint16_t i;
  Cpu *cpu = emu->cpu;
  Debugger *deb = emu->debugger;
  char str[100] = {0};

  for (i = 0;i < deb->num_bps;i++) {
    if (cpu->pc == deb->bp_addresses[i]) {
      sprintf(str, "\n\t[Breakpoint PC[%d] hit]\n\n", i + 1);
      print_console(emu, str);
      handle_breakpoint_hit(emu);
      return true;
    }
  }

  for (i = 0;i < deb->num_memory_bps;i++) {
    if (memory_get_flags_of_virtual_address(
        (void*)((uintptr_t)(deb->memory_bp_addresses[i]))) != 0) {
      sprintf(str, "\n\t[Breakpoint MEM[%d] hit]\n\n", i + 1);
      print_console(emu, str);
      handle_breakpoint_hit(emu);
      return true;
    }
  }
  return false;
}
