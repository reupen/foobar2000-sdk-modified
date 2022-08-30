#pragma once
#include <SDK/cfg_var.h>
#include <SDK/dsp.h>

class cfg_dsp_chain_config : public cfg_blob {
public:
	void reset() {
		cfg_blob::set(nullptr);
	}
	cfg_dsp_chain_config(const GUID& p_guid) : cfg_blob(p_guid) {}

	void get_data(dsp_chain_config& p_data) {
		p_data.from_blob(cfg_blob::get());
	}
	void set_data(const dsp_chain_config& p_data) {
		cfg_blob::set(p_data.to_blob());
	}
};

typedef cfg_dsp_chain_config cfg_dsp_chain_config_mt;