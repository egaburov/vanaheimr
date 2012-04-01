/*! \file   PTXToVIRTranslator.cpp
	\author Gregory Diamos <gregory.diamos@gatech.edu>
	\date   Saturday Fubruary 25, 2012
	\brief  The source file for the PTXToVIRTranslator class.
*/

// Vanaheimr Includes
#include <vanaheimr/translation/interface/PTXToVIRTranslator.h>

#include <vanaheimr/ir/interface/Type.h>

#include <vanaheimr/compiler/interface/Compiler.h>

// Ocelot Includes
#include <ocelot/ir/interface/Module.h>
#include <ocelot/ir/interface/PTXKernel.h>

// Hydrazine Includes
#include <hydrazine/interface/debug.h>

// Standard Library Includes
#include <stdexcept>

// Preprocessor Macros
#ifdef REPORT_BASE
#undef REPORT_BASE
#endif

#define REPORT_BASE 1

namespace vanaheimr
{

namespace translation
{

PTXToVIRTranslator::PTXToVIRTranslator(compiler::Compiler* compiler)
: _compiler(compiler)
{

}

void PTXToVIRTranslator::translate(const PTXModule& m)
{
	report("Translating PTX module '"  << m.path() << "'");

	_ptx    = &m;
	_module = &*_compiler->newModule(m.path());
	
	// Translate globals
	for(PTXModule::GlobalMap::const_iterator global = m.globals().begin();
		global != m.globals().end(); ++global)
	{
		_translateGlobal(global->second);
	}
	
	// Translate kernel functions
	for(PTXModule::KernelMap::const_iterator kernel = m.kernels().begin();
		kernel != m.kernels().end(); ++kernel)
	{
		_translateKernel(*kernel->second);
	}
}

void PTXToVIRTranslator::_translateGlobal(const PTXGlobal& global)
{
	report(" Translating PTX global " << global.statement.toString());
	
	ir::Module::global_iterator virGlobal = _module->newGlobal(
		global.statement.name, _getType(global.statement.type),
		_translateLinkage(global.statement.attribute));
		
	if(global.statement.initializedBytes() != 0)
	{
		virGlobal->setInitializer(_translateInitializer(global));
	}
}

void PTXToVIRTranslator::_translateKernel(const PTXKernel& kernel)
{
	report(" Translating PTX kernel '" << kernel.getPrototype().toString());

	ir::Module::iterator function = _module->newFunction(kernel.name,
		_translateLinkingDirective(kernel.getPrototype().linkingDirective));
	
	_function = &*function;
	
	// Translate Values
	PTXKernel::RegisterVector registers = kernel.getReferencedRegisters();
	
	for(PTXKernel::RegisterVector::iterator reg = registers.begin();
		reg != registers.end(); ++reg)
	{
		_translateRegisterValue(reg->id, reg->type);
	}
	
	::ir::ControlFlowGraph::ConstBlockPointerVector sequence =
		kernel.cfg()->executable_sequence();

	// Keep a record of blocks
	for(::ir::ControlFlowGraph::ConstBlockPointerVector::iterator
		block = sequence.begin(); block != sequence.end(); ++block)
	{
		if(*block == kernel.cfg()->get_entry_block()) continue;
		if(*block == kernel.cfg()->get_exit_block())  continue;
		_translateBasicBlock(**block);
	}
	
	// Translate blocks
	for(::ir::ControlFlowGraph::ConstBlockPointerVector::iterator
		block = sequence.begin(); block != sequence.end(); ++block)
	{
		if(*block == kernel.cfg()->get_entry_block()) continue;
		if(*block == kernel.cfg()->get_exit_block())  continue;

		_translateBasicBlock(**block);
	}
}

void PTXToVIRTranslator::_translateRegisterValue(PTXRegisterId reg,
	PTXDataType type)
{
	report("  Translating PTX register "
		<< PTXOperand::toString((PTXOperand::DataType) type)
		<< " r" << reg);

	std::stringstream name;
	
	name << "r"  << reg;
	
	if(_registers.count(reg) != 0)
	{
		throw std::runtime_error("Added duplicate virtual register '"
			+ name.str() + "'");
	}
	
	ir::Function::register_iterator newRegister = _function->newVirtualRegister(
		_getType(type), name.str());

	report("   to " << newRegister->type->name() << " r" << newRegister->id);

	_registers.insert(std::make_pair(reg, newRegister));
}

void PTXToVIRTranslator::_translateBasicBlock(const PTXBasicBlock& basicBlock)
{
	report("  Translating PTX basic block " << basicBlock.label);

	ir::Function::iterator block = _function->newBasicBlock(
		_function->exit_block(), basicBlock.label);

	_block = &*block;
	
	for(PTXBasicBlock::const_instruction_iterator
		instruction = basicBlock.instructions.begin();
		instruction != basicBlock.instructions.end(); ++instruction)
	{
		const PTXInstruction& ptx = static_cast<const PTXInstruction&>(
			**instruction);
	
		_translateInstruction(ptx);
	}
}

void PTXToVIRTranslator::_translateInstruction(const PTXInstruction& ptx)
{
	report("   Translating PTX instruction " << ptx.toString());

	_ptxInstruction = &ptx;

	// Translate complex instructions
	if(_translateComplexInstruction(ptx)) return;
	
	// Translate simple instructions
	if(_translateSimpleBinaryInstruction(ptx)) return;
	if(_translateSimpleUnaryInstruction(ptx))  return;
	
	assertM(false, "No translation implemented for instruction "
		<< ptx.toString());
}

bool PTXToVIRTranslator::_translateComplexInstruction(const PTXInstruction& ptx)
{
	return false;
}

static ir::UnaryInstruction* newUnaryInstruction(
	const ::ir::PTXInstruction& ptx)
{
	typedef ::ir::PTXInstruction PTXInstruction;
	typedef ::ir::PTXOperand     PTXOperand;
	
	switch(ptx.opcode)
	{
	case PTXInstruction::Ld: // fall through
	case PTXInstruction::Ldu:
	{
		return new ir::Ld;
	}
	case PTXInstruction::Mov:
	{
		return new ir::Bitcast;
	}
	case PTXInstruction::Cvt:
	{
		if(PTXOperand::isFloat(ptx.d.type))
		{
			if(PTXOperand::isFloat(ptx.a.type))
			{
				if(ptx.d.type == PTXOperand::f32)
				{
					if(ptx.a.type == PTXOperand::f32)
					{
						return new ir::Bitcast;
					}
					else
					{
						return new ir::Fptrunc;
					}
				}
				else
				{
					if(ptx.a.type == PTXOperand::f32)
					{
						return new ir::Fpext;
					}
					else
					{
						return new ir::Bitcast;
					}
				}
			}
			else if(PTXOperand::isSigned(ptx.a.type))
			{
				return new ir::Sitofp;
			}
			else
			{
				return new ir::Uitofp;
			}
		}
		else if(PTXOperand::isSigned(ptx.d.type))
		{
			if(PTXOperand::isFloat(ptx.a.type))
			{
				return new ir::Fptosi;
			}
			else
			{
				if(PTXOperand::bytes(ptx.a.type) >
					PTXOperand::bytes(ptx.d.type))
				{
					return new ir::Trunc;
				}
				else if(PTXOperand::bytes(ptx.d.type) ==
					PTXOperand::bytes(ptx.a.type))
				{
					return new ir::Bitcast;
				}
				else if(PTXOperand::isSigned(ptx.a.type))
				{
					return new ir::Sext;
				}
				else
				{
					return new ir::Zext;
				}
			}
		}
		else
		{
			if(PTXOperand::isFloat(ptx.a.type))
			{
				return new ir::Fptoui;
			}
			else
			{
				if(PTXOperand::bytes(ptx.a.type) >
					PTXOperand::bytes(ptx.d.type))
				{
					return new ir::Trunc;
				}
				else if(PTXOperand::bytes(ptx.d.type) ==
					PTXOperand::bytes(ptx.a.type))
				{
					return new ir::Bitcast;
				}
				else
				{
					return new ir::Zext;
				}
			}
		}
		break;
	}
	default:
	{
		break;
	}
	}
	
	return 0;	
}

static bool isSimpleUnaryInstruction(const ::ir::PTXInstruction& ptx)
{
	typedef ::ir::PTXInstruction PTXInstruction;

	switch(ptx.opcode)
	{
	case PTXInstruction::Ld:
	case PTXInstruction::Ldu:
	case PTXInstruction::Mov:
	case PTXInstruction::St:
	{
		return true;
		break;
	}
	case PTXInstruction::Cvt:
	{
		if(ptx.modifier == PTXInstruction::Modifier_invalid)
		{
			return true;
		}
		break;
	}
	default:
	{
		break;
	}
	}
	
	return false;
}

bool PTXToVIRTranslator::_translateSimpleUnaryInstruction(
	const PTXInstruction& ptx)
{
	if(!isSimpleUnaryInstruction(ptx)) return false;

	ir::UnaryInstruction* vir = newUnaryInstruction(ptx);
	_instruction = vir;
	
	vir->setGuard(_translatePredicateOperand(ptx.pg));
	vir->setD(_newTranslatedOperand(ptx.d));
	vir->setA(_newTranslatedOperand(ptx.a));

	report("   to " << vir->toString());
	
	_block->push_back(vir);
	
	return true;
}

static bool isSimpleBinaryInstruction(const ::ir::PTXInstruction& ptx)
{
	typedef ::ir::PTXInstruction PTXInstruction;

	switch(ptx.opcode)
	{
	case PTXInstruction::Add: // fall through
	case PTXInstruction::And: // fall through
	case PTXInstruction::Div: // fall through
	case PTXInstruction::Mul: // fall through
	case PTXInstruction::Not: // fall through
	case PTXInstruction::Or:  // fall through
	case PTXInstruction::Rem: // fall through
	case PTXInstruction::Shl: // fall through
	case PTXInstruction::Sub: // fall through
	case PTXInstruction::Xor:
	{
		return true;
	}
	default:
	{
		break;
	}
	}
	
	return false;
}

static ir::BinaryInstruction* newBinaryInstruction(
	const ::ir::PTXInstruction& ptx)
{
	typedef ::ir::PTXInstruction PTXInstruction;
	typedef ::ir::PTXOperand     PTXOperand;
	
	switch(ptx.opcode)
	{
	case PTXInstruction::Add:
	{
		return new ir::Add;
	}
	case PTXInstruction::And:
	{
		return new ir::And;		
	}
	case PTXInstruction::Div:
	{
		if(PTXOperand::isFloat(ptx.type))
		{
			return new ir::Fdiv;
		}
		else
		{
			if(PTXOperand::isSigned(ptx.type))
			{
				return new ir::Sdiv;
			}
			else
			{
				return new ir::Udiv;
			}
		}
	}
	case PTXInstruction::Mul:
	{
		if(PTXOperand::isFloat(ptx.type))
		{
			return new ir::Fmul;
		}
		else
		{
			return new ir::Mul;
		}
	}
	case PTXInstruction::Or:
	{
		return new ir::Or;		
	}
	case PTXInstruction::Rem:
	{
		if(PTXOperand::isFloat(ptx.type))
		{
			return new ir::Frem;
		}
		else
		{
			if(PTXOperand::isSigned(ptx.type))
			{
				return new ir::Srem;
			}
			else
			{
				return new ir::Urem;
			}
		}
	}
	case PTXInstruction::Shl:
	{
		return new ir::Shl;		
	}
	case PTXInstruction::Sub:
	{
		return new ir::Sub;		
	}
	case PTXInstruction::Xor:
	{
		return new ir::Xor;		
	}
	default:
	{
		break;
	}
	}
	
	return 0;
}

bool PTXToVIRTranslator::_translateSimpleBinaryInstruction(
	const PTXInstruction& ptx)
{
	if(!isSimpleBinaryInstruction(ptx)) return false;
	
	ir::BinaryInstruction* vir = newBinaryInstruction(ptx);
	
	vir->setGuard(_translatePredicateOperand(ptx.pg));
	vir->setD(_newTranslatedOperand(ptx.d));
	vir->setA(_newTranslatedOperand(ptx.a));
	vir->setB(_newTranslatedOperand(ptx.b));
	
	report("   to " << vir->toString());

	_block->push_back(vir);
	
	return true;
}

ir::Operand* PTXToVIRTranslator::_newTranslatedOperand(const PTXOperand& ptx)
{
	switch(ptx.addressMode)
	{
	case PTXOperand::Register:
	{
		return new ir::RegisterOperand(_getRegister(ptx.reg), _instruction);
	}
	case PTXOperand::Indirect:
	{
		return new ir::IndirectOperand(_getRegister(ptx.reg),
			ptx.offset, _instruction);
	}
	case PTXOperand::Immediate:
	{
		return new ir::ImmediateOperand((uint64_t)ptx.imm_uint, _instruction);
	}
	case PTXOperand::Address:
	{
		if(_ptxInstruction->addressSpace == PTXInstruction::Param &&
			ptx.isArgument)
		{
			return new ir::ArgumentOperand(_getArgument(ptx.identifier),
				_instruction);
		}
		else
		{
			return new ir::AddressOperand(
				_getGlobal(ptx.identifier), _instruction);
		}
	}
	case PTXOperand::Label:
	{
		return new ir::AddressOperand(_getBasicBlock(ptx.identifier),
			_instruction);
	}
	case PTXOperand::Special:
	{
		return _getSpecialValueOperand(ptx.special, ptx.vIndex);
	}
	case PTXOperand::BitBucket:
	{
		return new ir::RegisterOperand(_newTemporaryRegister(), _instruction);
	}
	default: break;
	}
	
	throw std::runtime_error("No translation implemented for "
		+ ptx.toString());
}

static ir::PredicateOperand::PredicateModifier translatePredicateCondition(
	::ir::PTXOperand::PredicateCondition c)
{
	switch(c)
	{
	case ::ir::PTXOperand::PT:
	{
		return ir::PredicateOperand::PredicateTrue;
	}
	case ::ir::PTXOperand::nPT:
	{
		return ir::PredicateOperand::PredicateFalse;
	}
	case ::ir::PTXOperand::Pred:
	{
		return ir::PredicateOperand::StraightPredicate;
	}
	case ::ir::PTXOperand::InvPred:
	{
		return ir::PredicateOperand::InversePredicate;
	}
	}

	return ir::PredicateOperand::StraightPredicate;
}

ir::PredicateOperand* PTXToVIRTranslator::_translatePredicateOperand(
	const PTXOperand& ptx)
{
	ir::VirtualRegister* predicateRegister = 0;

	if(ptx.condition != PTXOperand::PT && ptx.condition != PTXOperand::nPT)
	{
		predicateRegister = _getRegister(ptx.reg);
	}
	
	return new ir::PredicateOperand(predicateRegister,
		translatePredicateCondition(ptx.condition), _instruction);
}

ir::VirtualRegister* PTXToVIRTranslator::_getSpecialVirtualRegister(
	unsigned int id, unsigned int vectorIndex)
{
	unsigned int hash = (id << 4) | vectorIndex;

	RegisterMap::iterator reg = _specialRegisters.find(hash);

	if(reg == _specialRegisters.end())
	{
		std::stringstream stream;
	
		bool isScalar = true;
		switch (id) 
		{
		case PTXOperand::tid: // fall through
		case PTXOperand::ntid: // fall through
		case PTXOperand::ctaId: // fall through
		case PTXOperand::nctaId:  // fall through
		case PTXOperand::smId:  // fall through
		case PTXOperand::nsmId:  // fall through
		case PTXOperand::gridId:  // fall through
			isScalar = false;
			break;
		default:
			isScalar = true;
		}
		
		if(vectorIndex != PTXOperand::v1 || isScalar) 
		{
			stream << PTXOperand::toString((PTXOperand::SpecialRegister)id);
		}
		else
		{
			stream << PTXOperand::toString((PTXOperand::SpecialRegister)id) +
				"_" + PTXOperand::toString((PTXOperand::VectorIndex)vectorIndex);
		}

		ir::Function::register_iterator newRegister =
			_function->newVirtualRegister(_getType("i32"), stream.str());
		reg = _specialRegisters.insert(std::make_pair(hash, newRegister)).first;
	}

	return &*reg->second;
}

ir::VirtualRegister* PTXToVIRTranslator::_getRegister(PTXRegisterId id)
{
	RegisterMap::iterator reg = _registers.find(id);
	
	if(reg == _registers.end())
	{
		std::stringstream name;
		
		name << "r" << id;

		throw std::runtime_error("PTX register " + name.str()
			+ " used without declaration.");
	}
	
	return &*reg->second;
}

ir::Variable* PTXToVIRTranslator::_getGlobal(const std::string& name)
{
	ir::Module::global_iterator global = _module->getGlobal(name);
	
	if(global == _module->global_end())
	{
		throw std::runtime_error("Global variable " + name
			+ " used without declaration.");
	}
	
	return &*global;
}

ir::Variable* PTXToVIRTranslator::_getBasicBlock(const std::string& name)
{
	BasicBlockMap::iterator block = _blocks.find(name);
	
	if(block == _blocks.end())
	{
		throw std::runtime_error("Basic block " + name
			+ " was not declared in this function.");
	}

	return &*block->second;
}

ir::Argument* PTXToVIRTranslator::_getArgument(const std::string& name)
{
	for(ir::Function::argument_iterator argument = _function->argument_begin();
		argument != _function->argument_end(); ++argument)
	{
		if(argument->name() == name) return &*argument;
	}
	
	throw std::runtime_error("Argument " + name
		+ " was not declared in this function.");
		
	return nullptr;
}

ir::Operand* PTXToVIRTranslator::_getSpecialValueOperand(unsigned int id, unsigned int vIndex)
{
	return new ir::RegisterOperand(_getSpecialVirtualRegister(id, vIndex), _instruction);
}

ir::VirtualRegister* PTXToVIRTranslator::_newTemporaryRegister()
{
	ir::Function::register_iterator temp = _function->newVirtualRegister(
		_getType("i64"));
		
	return &*temp;
}

static std::string translateTypeName(::ir::PTXOperand::DataType type)
{
	switch(type)
	{
	case ::ir::PTXOperand::b8:  /* fall through */
	case ::ir::PTXOperand::s8:  /* fall through */
	case ::ir::PTXOperand::u8:
	{
		return "i8";
	}
	case ::ir::PTXOperand::s16: /* fall through */
	case ::ir::PTXOperand::u16: /* fall through */
	case ::ir::PTXOperand::b16:
	{
		return "i16";
	}
	case ::ir::PTXOperand::s32: /* fall through */
	case ::ir::PTXOperand::b32: /* fall through */
	case ::ir::PTXOperand::u32:
	{
		return "i32";
	}
	case ::ir::PTXOperand::s64: /* fall through */
	case ::ir::PTXOperand::b64: /* fall through */
	case ::ir::PTXOperand::u64:
	{
		return "i64";
	}
	case ::ir::PTXOperand::f32:
	{
		return "f32";
	}
	case ::ir::PTXOperand::f64:
	{
		return "f64";
	}
	case ::ir::PTXOperand::pred:
	{
		return "i1";
	}
	default: break;
	}
	
	return "";
}

const ir::Type* PTXToVIRTranslator::_getType(PTXDataType ptxType)
{
	return _getType(translateTypeName((::ir::PTXOperand::DataType)ptxType));
}

const ir::Type* PTXToVIRTranslator::_getType(const std::string& typeName)
{
	const ir::Type* type = _compiler->getType(typeName);

	if(type == 0)
	{
		throw std::runtime_error("PTX translated type name '"
			+ typeName + "' is not a valid Vanaheimr type.");
	}
	
	return type;
}

ir::Variable::Linkage PTXToVIRTranslator::_translateLinkage(PTXAttribute attr)
{
	if(attr == ::ir::PTXStatement::Extern)
	{
		return ir::Variable::ExternalLinkage;
	}
	else
	{
		return ir::Variable::PrivateLinkage;
	}
}

ir::Variable::Linkage PTXToVIRTranslator::_translateLinkingDirective(
	PTXLinkingDirective d)
{
	if(d == PTXKernel::Prototype::Extern)
	{
		return ir::Variable::ExternalLinkage;
	}
	else
	{
		return ir::Variable::PrivateLinkage;
	}
}

ir::Constant* PTXToVIRTranslator::_translateInitializer(const PTXGlobal& g)
{
	assertM(false, "Not implemented.");
	
	return 0;
}

}

}

