#include "DfgGenInternal.h"

void AllocateSPMBanks(std::unordered_set<Value *> &outer_vals,
		std::unordered_map<Value *, GetElementPtrInst *> &mem_ptrs,
		std::unordered_map<Value *, int> &acc,
		std::unordered_map<Value *, SPM_BANK> &spm_bank_allocation,
		std::unordered_map<Value *, int> &spm_base_address,
		Function &F,
		unsigned &required_bank_size_out,
		unsigned &effective_bank_size_out)
{


	LLVM_DEBUG(dbgs()<<"number of banks: "<<banks_number<<"\n");
	LLVM_DEBUG(dbgs()<<"banks size: "<<bank_size<<"\n");
	LLVM_DEBUG(dbgs()<<"Data placement policy: "<<dp_policy<<"\n");
	// Find variable sizes;
	std::unordered_map<Value *, int> variable_sizes_bytes;
	DataLayout DL = F.getParent()->getDataLayout();
	unsigned required_bank_size = bank_size;

	//for outer vals
	LLVM_DEBUG(dbgs()<<"For outer values \n");
	for (auto it = outer_vals.begin(); it != outer_vals.end(); it++)
	{
		Value *outer_val = *it;
		LLVM_DEBUG(outer_val->dump());
		int size = DL.getTypeAllocSize(outer_val->getType());
		LLVM_DEBUG(dbgs() <<" Size:" << size << "/n");
		required_bank_size = std::max(required_bank_size, static_cast<unsigned>(size));
		variable_sizes_bytes[outer_val] = size;
	}

	// for mem ptrs
	for (auto it = mem_ptrs.begin(); it != mem_ptrs.end(); it++)
	{
		Value *mem_value = it->first;
		GetElementPtrInst *gep = it->second;
		std::string gep_pointer_name = gep->getPointerOperand()->getName().str();
		if (sizeArrMap.find(gep_pointer_name) != sizeArrMap.end())
		{
			variable_sizes_bytes[gep->getPointerOperand()] = sizeArrMap[gep_pointer_name];
			required_bank_size = std::max(
				required_bank_size,
				static_cast<unsigned>(variable_sizes_bytes[gep->getPointerOperand()]));
			LLVM_DEBUG(dbgs() << gep_pointer_name << ", size = " << sizeArrMap[gep_pointer_name] << "\n");
		}
		else
		{
			int size = DL.getTypeAllocSize(gep->getSourceElementType());
			variable_sizes_bytes[gep->getPointerOperand()] = size;
			required_bank_size = std::max(required_bank_size, static_cast<unsigned>(size));
			LLVM_DEBUG(dbgs() << gep_pointer_name << ", size = " << size << "\n");
		}
	}

	for (auto it = acc.begin(); it != acc.end(); it++)
	{
		Value *base_ptr = it->first;
		if (variable_sizes_bytes.find(base_ptr) != variable_sizes_bytes.end())
			continue;

		std::string base_name = base_ptr->getName().str();
		if (sizeArrMap.find(base_name) != sizeArrMap.end())
		{
			variable_sizes_bytes[base_ptr] = sizeArrMap[base_name];
		}
		else if (base_ptr->getType()->isPointerTy())
		{
			if (GlobalVariable *GV = dyn_cast<GlobalVariable>(base_ptr))
			{
				variable_sizes_bytes[base_ptr] = DL.getTypeAllocSize(GV->getValueType());
			}
			else
			{
				// Opaque-pointer fallback: we may no longer know the aggregate type.
				// Use one 32-bit element, matching the pre-existing GEP-based fallback
				// that sized unknown arrays by accessed element width rather than full extent.
				variable_sizes_bytes[base_ptr] = 4;
			}
		}
		else
		{
			variable_sizes_bytes[base_ptr] = DL.getTypeAllocSize(base_ptr->getType());
		}
		required_bank_size = std::max(
			required_bank_size,
			static_cast<unsigned>(variable_sizes_bytes[base_ptr]));
	}



	std::vector<std::unordered_set<Value*>> banks_vars;
	std::map<Value*, int> value_to_BankId;
	std::map<int, int> data_in_bank;
	for(int i = 0; i < banks_number ; i++){
		std::unordered_set<Value*> temp;
		banks_vars.push_back(temp);
		data_in_bank [i] = 0;
	}


	//data placement
	/*assign acc (arrays and scalars) to memories. It balances the data amount (size) of each bank.
	Currently don't consider number of accesses for each array. */
	{

		int desired_bank = 0;
		for(auto it = acc.begin(); it != acc.end(); it++){
			int size = variable_sizes_bytes[it->first];
			int least_data = data_in_bank[0];
			if(dp_policy ==0){ //Data placement policy 0 : balances the data amount (size) of each bank.
				desired_bank = 0;
				for(int i = 0 ; i< banks_number;i++){
					if(data_in_bank [i] < least_data ){
						least_data = data_in_bank [i];
						desired_bank = i;
					}
				}

			}else{ //Data placement policy 1 : place arrays on alternative banks

				if(desired_bank == banks_number-1){
					desired_bank = 0;
				}else{
					desired_bank = desired_bank + 1;
				}
			}

			LLVM_DEBUG(dbgs()<<"assign"<< size << "to bank"<<desired_bank<<"\n");
			banks_vars[desired_bank].insert(it->first);
			value_to_BankId[it->first] = desired_bank;
			data_in_bank[desired_bank] = size + data_in_bank[desired_bank];
			required_bank_size = std::max(
				required_bank_size,
				static_cast<unsigned>(data_in_bank[desired_bank]));

		}
	}


	for(int i = 0; i < banks_vars.size(); i++){
		auto & bank_vars = banks_vars[i];
		LLVM_DEBUG(dbgs() << "Bank"<<i<< " vars :: \n");
		for(Value* v : bank_vars){
			LLVM_DEBUG(dbgs() << "\t" << v->getName() << " :: size = " << variable_sizes_bytes[v] << ", acceses = " << acc[v] << "\n");
			// spm_bank_allocation[v]=BANK0;
			// spm_base_address[v] = bank0_addr;
			// bank0_addr += variable_sizes_bytes[v];
		}
	}

	unsigned effective_bank_size = std::max(static_cast<unsigned>(bank_size), required_bank_size);
	int mem_size = banks_number * effective_bank_size;
	required_bank_size_out = required_bank_size;
	effective_bank_size_out = effective_bank_size;

	if (required_bank_size > bank_size)
	{
		errs() << "Required SPM bank size for provisional layout: "
		       << required_bank_size
		       << " bytes (configured: "
		       << static_cast<unsigned>(bank_size)
		       << " bytes).\n";
		errs() << "Continuing DFG generation with effective bank size: "
		       << effective_bank_size
		       << " bytes.\n";
	}

	LLVM_DEBUG(dbgs() << "FINAL ALLOCATION BEGIN.\n");
	std::map<int, int> bank_base_address;
	for(int i = 0; i< banks_number;i++){
		bank_base_address.emplace(i, i * effective_bank_size);
	}
	for (auto it = mem_ptrs.begin(); it != mem_ptrs.end(); it++)
	{
		Value* mem_ins = it->first;
		GetElementPtrInst* gep = it->second;

		LLVM_DEBUG(dbgs() << "pointer_ins = " << mem_ins->getName() << ",");
		LLVM_DEBUG(dbgs()<< "gep_pointer = " << gep->getPointerOperand()->getName() << ",");
		LLVM_DEBUG(dbgs() << "size = " << variable_sizes_bytes[gep->getPointerOperand()] << ",");

		if(value_to_BankId.find(gep->getPointerOperand()) != value_to_BankId.end()){
			if(spm_base_address.find(gep->getPointerOperand()) == spm_base_address.end()){
				int bank_id = (value_to_BankId.find(gep->getPointerOperand()))->second;
				spm_bank_allocation[gep->getPointerOperand()] = SPMBANKOfIndex(bank_id);
				LLVM_DEBUG(dbgs() << "bank="<<bank_id<<",");
				spm_base_address[gep->getPointerOperand()] = bank_base_address[bank_id];
				assert(variable_sizes_bytes.find(gep->getPointerOperand()) != variable_sizes_bytes.end());
				bank_base_address[bank_id] += variable_sizes_bytes[gep->getPointerOperand()];
				LLVM_DEBUG(dbgs() << "addr=" << spm_base_address[gep->getPointerOperand()] << "\n");
			}
			else{
				LLVM_DEBUG(dbgs() << "array/struct already allocated \n");
			}
		}
		else{
			DFG_CHECK(false, "AllocateSPMBanks",
				"GEP pointer '" + gep->getPointerOperand()->getName().str() +
				"' has no assigned SPM bank; check data placement configuration");
		}
	}

	for (auto it = outer_vals.begin(); it != outer_vals.end(); it++)
	{
		Value* outer_value_mem = *it;

		LLVM_DEBUG(dbgs() << "outer value = " << outer_value_mem->getName() << ",");
		if(value_to_BankId.find(outer_value_mem) != value_to_BankId.end()){
			int bank_id = (value_to_BankId.find(outer_value_mem))->second;
			spm_bank_allocation[outer_value_mem] = SPMBANKOfIndex(bank_id);
			spm_base_address[outer_value_mem] = bank_base_address[bank_id];
			assert(variable_sizes_bytes.find(outer_value_mem) != variable_sizes_bytes.end());
			bank_base_address[bank_id] += variable_sizes_bytes[outer_value_mem];

			LLVM_DEBUG(dbgs() << "bank="<<bank_id<<",");
			LLVM_DEBUG(dbgs() << "addr=" << spm_base_address[outer_value_mem] << "\n");
		}
		else{
			DFG_CHECK(false, "AllocateSPMBanks",
				"outer value '" + outer_value_mem->getName().str() +
				"' has no assigned SPM bank; check data placement configuration");
		}

	}

	for (auto it = value_to_BankId.begin(); it != value_to_BankId.end(); it++)
	{
		Value *base_ptr = it->first;
		if (spm_base_address.find(base_ptr) != spm_base_address.end())
			continue;

		int bank_id = it->second;
		spm_bank_allocation[base_ptr] = SPMBANKOfIndex(bank_id);
		spm_base_address[base_ptr] = bank_base_address[bank_id];
		assert(variable_sizes_bytes.find(base_ptr) != variable_sizes_bytes.end());
		bank_base_address[bank_id] += variable_sizes_bytes[base_ptr];
	}

	LLVM_DEBUG(dbgs() << "FINAL ALLOCATION END.\n");


	// assert(false);

}
