#include "ArtifactSim/DFG.h"

#include "morpher/util/tinyxml2.h"

#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace ArtifactSim {

using tinyxml2::XMLElement;
using tinyxml2::XMLDocument;

static std::string readTextFile(const std::string &path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("could not open DFG XML: " + path);
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

static std::string normalizeXml(const std::string &text) {
  return std::regex_replace(text, std::regex("\"([A-Za-z_][A-Za-z0-9_:-]*=)"), "\" $1");
}

static std::string childText(XMLElement *elem, const char *name) {
  XMLElement *child = elem->FirstChildElement(name);
  if (!child || !child->GetText()) {
    return "";
  }
  return child->GetText();
}

static std::vector<EdgeRef> parseRefs(XMLElement *node, const char *containerName, const char *refName) {
  std::vector<EdgeRef> refs;
  XMLElement *container = node->FirstChildElement(containerName);
  if (!container) {
    return refs;
  }
  for (XMLElement *ref = container->FirstChildElement(refName); ref; ref = ref->NextSiblingElement(refName)) {
    EdgeRef edge;
    ref->QueryIntAttribute("idx", &edge.idx);
    const char *type = ref->Attribute("type");
    if (type) {
      edge.type = type;
    }
    int nextIter = 0;
    ref->QueryIntAttribute("nextiter", &nextIter);
    edge.nextIter = nextIter != 0;
    int npb = 0;
    ref->QueryIntAttribute("NPB", &npb);
    edge.npb = npb != 0;
    refs.push_back(edge);
  }
  return refs;
}

int opAccessSize(const std::string &op) {
  if (op.size() >= 1 && op.back() == 'B') return 1;
  if (op.size() >= 1 && op.back() == 'H') return 2;
  return 4;
}

bool isLoadOp(const std::string &op) {
  return op.find("LOAD") != std::string::npos;
}

bool isStoreOp(const std::string &op) {
  return op.find("STORE") != std::string::npos;
}

DFGGraph DFGGraph::parse(const std::string &path) {
  std::string wrapped = "<Root>\n" + normalizeXml(readTextFile(path)) + "\n</Root>";
  XMLDocument doc;
  tinyxml2::XMLError err = doc.Parse(wrapped.c_str(), wrapped.size());
  if (err != tinyxml2::XML_SUCCESS) {
    throw std::runtime_error("failed to parse DFG XML: " + path);
  }
  XMLElement *root = doc.FirstChildElement("Root");
  XMLElement *dfg = root ? root->FirstChildElement("DFG") : nullptr;
  if (!dfg) {
    throw std::runtime_error("DFG XML missing <DFG>: " + path);
  }

  DFGGraph graph;
  for (XMLElement *elem = dfg->FirstChildElement("Node"); elem; elem = elem->NextSiblingElement("Node")) {
    DFGNode node;
    elem->QueryIntAttribute("idx", &node.idx);
    elem->QueryIntAttribute("ASAP", &node.asap);
    elem->QueryIntAttribute("ALAP", &node.alap);
    node.op = childText(elem, "OP");
    int constInt = 0;
    if (elem->QueryIntAttribute("CONST", &constInt) == tinyxml2::XML_SUCCESS) {
      node.hasConst = true;
      node.constValue = static_cast<uint32_t>(constInt);
    }
    const char *packed = elem->Attribute("simd_packed");
    node.simdPacked = packed && std::string(packed) == "true";
    elem->QueryIntAttribute("simd_lanes", &node.simdLanes);
    if (node.simdLanes < 1) {
      node.simdLanes = 1;
    }
    const char *reduceOp = elem->Attribute("simd_reduce_op");
    if (reduceOp) {
      node.simdReduceOp = reduceOp;
    }
    const char *reduceConsumer = elem->Attribute("simd_reduce_consumer");
    node.simdReduceConsumer = reduceConsumer && std::string(reduceConsumer) == "true";

    XMLElement *base = elem->FirstChildElement("BasePointerName");
    if (base && base->GetText()) {
      node.mem.base = base->GetText();
      int declaredSize = 0;
      base->QueryIntAttribute("size", &declaredSize);
      (void)declaredSize;
    }
    XMLElement *gep = elem->FirstChildElement("GEPOffset");
    if (gep && gep->GetText()) {
      node.mem.hasGepOffset = true;
      node.mem.gepOffset = std::stoi(gep->GetText());
    }
    node.mem.sizeBytes = opAccessSize(node.op);
    node.inputs = parseRefs(elem, "Inputs", "Input");
    node.outputs = parseRefs(elem, "Outputs", "Output");
    node.recParents = parseRefs(elem, "RecParents", "RecParent");
    graph.nodes_[node.idx] = node;
  }
  for (auto &consumerEntry : graph.nodes_) {
    DFGNode &consumer = consumerEntry.second;
    for (EdgeRef &input : consumer.inputs) {
      auto producerIt = graph.nodes_.find(input.idx);
      if (producerIt == graph.nodes_.end()) {
        continue;
      }
      for (const EdgeRef &output : producerIt->second.outputs) {
        if (output.idx != consumer.idx) {
          continue;
        }
        input.type = output.type;
        input.nextIter = output.nextIter;
        input.npb = output.npb;
        break;
      }
    }
  }
  return graph;
}

const std::map<int, DFGNode> &DFGGraph::nodes() const {
  return nodes_;
}

const DFGNode &DFGGraph::node(int idx) const {
  auto it = nodes_.find(idx);
  if (it == nodes_.end()) {
    throw std::runtime_error("DFG node not found: " + std::to_string(idx));
  }
  return it->second;
}

} // namespace ArtifactSim
