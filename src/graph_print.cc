/* This file is part of onnx2c
 */

#include "error.h"
#include "graph.h"
#include "options.h"
#include "util.h"

#include <iostream>

using namespace toC;

void Graph::print_header(std::ostream &dst)
{
	print_file_frontmatter(dst);
}

void Graph::print_source(std::ostream &dst)
{
	print_file_frontmatter(dst);
	dst << std::endl;
	print_includes(dst);
	dst << std::endl;
	print_global_tensors(dst);
	dst << std::endl;
	print_functions(dst);
	dst << std::endl;
	print_interface_function(dst);
}


void Graph::print_file_frontmatter(std::ostream &dst)
{
	dst << "// This file is computer-generated by onnx2c " << std::endl;
	dst << "// (TODO: add creating command line here)" << std::endl;
	dst << "// (TODO: print creation date here )" << std::endl;
	dst << std::endl;
	dst << "// ONNX model:" << std::endl;
	dst << "// produced by " << model.producer_name();
	dst << ", version " << model.producer_version() << std::endl;
	dst << "// ONNX IR version: " << onnx_ir_version() << std::endl;
	dst << "// Model documentation: " << std::endl;
	// TODO: beware & check for maliciously formatted doc strings!!!
	// (and when you do that, also append "//" to every newlin in the doc_string for nicer printing :)
	dst << "/*" << std::endl << model.doc_string() << std::endl << "*/" << std::endl;
}

void Graph::print_tensor(const Tensor *t, std::ostream &dst)
{
	if( t->generate == false )
		return;
	if( t->name == "" )
		return;
	if( t->data_dim.size() == 0 )
		ERROR("Tensor of no dimensions?");
	// This case has been seen in the wild. Not sure why it happens
	if( t->data_dim.size() == 1 && t->data_dim[0]==0 ){
		LOG(WARNING) << "Tensor " << t->name << " has size of 0. Skipping it" << std::endl;
		return;
	}

	if( t->union_no < 0 )
		dst << "static ";

	t->print_tensor(dst);
	if( t->initialize ) {
		if( options.target_avr && t->isConst )
			dst << " PROGMEM";
		dst << " = "<<std::endl;
		t->print_tensor_initializer(dst);
	}
	dst << ";" << std::endl;
}

void Graph::print_global_tensors(std::ostream &dst)
{
	// ununionized tensors
	LOG(TRACE) << "printing global tensors - ununionized " << std::endl;
	for( auto t : tensors )
	{
		LOG(TRACE) << "\t" << t->print_trace_dump() << std::endl;
		if( t->union_no < 0
		 && t->generate)
			print_tensor(t, dst);
	}

	LOG(TRACE) << "printing global tensors - unionized " << std::endl;
	for( unsigned u=0; u<tensor_unions.size(); u++ )
	{
		dst << "union tensor_union_" << u << " {" << std::endl;
		for( auto t : tensors )
		{
			if( t->union_no == static_cast<int32_t>(u))
				print_tensor(t, dst);
		}
		dst << "};" <<std::endl;
		dst << "static union tensor_union_" << u << " tu" << u << ";" << std::endl <<std::endl;
	}
	LOG(TRACE) << "(done printing global tensors)"<< std::endl;
}

void Graph::print_functions(std::ostream &dst)
{
	for( auto n : nodes ) {
		// handle meta-nodes separately
		if( n->op_name == "graph_io" )
			continue;
		dst << "/*" << std::endl;
		dst << " * Operand:           " << n->op_name << std::endl;
		dst << " * Name in ONNX file: " << n->onnx_name << std::endl;
		dst << " */" << std::endl;
		dst << "FUNC_PREFIX void ";
		dst << n->c_name() << "( ";
		n->print_function_parameters_definition(dst);
		dst << " )";
		dst <<  std::endl << "{" << std::endl;

		n->print(dst);

		dst << "}" << std::endl << std::endl;
	}
}

void Graph::print_includes(std::ostream &dst)
{
	dst << "#include <float.h>" << std::endl;
	dst << "#include <math.h>" << std::endl;
	dst << "#include <stdbool.h>" << std::endl;
	dst << "#include <stdint.h>" << std::endl;
	dst << "#include <string.h>" << std::endl;
	dst << std::endl;

	dst << "#define MAX(X,Y) ( X > Y ? X : Y)" << std::endl;
	dst << "#define MIN(X,Y) ( X < Y ? X : Y)" << std::endl;
	dst << "#define CLIP(X,L) ( MAX(MIN(X,L), -L) )" << std::endl;
	dst << std::endl;

 	// 'inline' functions are a C99 addition.
	dst << "#if __STDC_VERSION__ < 199901L" << std::endl;
	dst << "#define FUNC_PREFIX" << std::endl;
	dst << "#else" << std::endl;
	dst << "#define FUNC_PREFIX static inline" << std::endl;
	dst << "#endif" << std::endl;

	if( options.target_avr ) {
		dst << "#include <avr/pgmspace.h>" << std::endl;
		dst << "#define RD_PROGMEM(x) pgm_read_byte(&(x));" << std::endl;
	}
}

void Graph::print_interface_function(std::ostream &dst, bool definition)
{
	bool isfirst = true;
	// TODO: take the interface function name from the ONNX file name
	dst << "void entry(" ;
	for ( auto i : model.graph().input() ) {
		/* TODO: FIXME: separate input tensors that are initialized
		 * or re-initializable (and therefore count as input), from
		 * the "actual" input data */
		Tensor *t=findTensor(i.name());

		if( t && t->isIO ) {
			if(!isfirst)
				dst << ", ";
			else
				isfirst = false;

			t->print_tensor_as_const(dst);
		}
	}

	// find the graph output node
	// loop through the output nodes' inputs, printing them
	Node *graph_out_node = findNodeByName("graph_output");
	if( graph_out_node == nullptr )
		ERROR("internal onnx2c error: no graph_output node");

	for( unsigned o=0; o<graph_out_node->get_number_of_inputs(); o++)
	{
		Tensor *t = graph_out_node->get_input_tensor(o);

		if( t ) {
			if(!isfirst)
				dst << ", ";
			else
				isfirst = false;

			// kludge... in contrived cases (like unit tests), the graph can have a constant vector as its ouput.
			// Since this is the last function we write anyway...
			t->isConst = false;
			t->print_tensor(dst);
		}
	}

	dst << ")";
	if( !definition ) { // not definition, i.e. decalaration
		dst << ";" << std::endl;
		return;
	}

	// else: definition - print the rest
	dst << "{" << std::endl;

	// since nodes were resolved from graph inputs in the order there were
	// node inputs resolved, the nodes vector is now sorted in order so that
	// we don't need to check dependancies :)
	for( auto n : nodes )
	{
		// handle meta-nodes separately
		if( n->op_name == "graph_io" )
			continue;

		dst << "\t" << n->c_name() << "( ";
		n->print_function_parameters_callsite(dst);
		dst << ");" << std::endl;
	}

	dst << "}" << std::endl;
}
