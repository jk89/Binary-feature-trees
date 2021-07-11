#include <opencv2/opencv.hpp>
#include <opencv2/core/mat.hpp>
#include <thread>
#include <nlohmann/json.hpp>

// for convenience
using json = nlohmann::json;
using namespace std;

class Node
{
public:
    Node *parent;
    Node *root;
    vector<Node> children;
    vector<int> centroids;
    vector<int> id;
    // bool isLeafNode = false;
    Node(vector<int> id, vector<int> centroids, Node *parent, Node *root)
    {
        this->centroids = centroids;
        this->parent = parent;
        this->id = id;
        this->root = root;
    }
    json serialise() {}
};

class ComputeNode
{
public:
    vector<int> id;
    ComputeNode *parent;
    ComputeNode *root;
    vector<uint8_t> feature;
    int feature_id = -1;
    vector<ComputeNode> children;
    bool isRoot = false;
    bool isLeaf = false;
    // std::shared_ptr<FeatureMatrix> data,
    ComputeNode()
    {
        this->parent = nullptr;
        this->id = {};
        this->root = this;
        this->isRoot = true;
    }
    ComputeNode(vector<int> id, int feature_id, ComputeNode *parent, ComputeNode *root)
    {
        this->parent = parent;
        this->id = id;
        this->root = root;
        this->feature_id = feature_id;
    }
};

class TrainingNode : public Node
{
public:
    TrainingNode *parent;
    TrainingNode *root;
    map<int, vector<int>> clusterMembers = {};
    int k = 0;
    std::shared_ptr<FeatureMatrix> data;
    vector<int> level_data_indices = {};
    vector<TrainingNode> children = {};
    string vocTreeFile;
    long long currentPermutationCost = LLONG_MAX;
    int processor_count = 1;

    void fit_level()
    {
        if (this->finished == true)
            return;

        // if centroid seeds are incomplete
        if (this->centroids.size() != this->k)
        {
            this->centroids = seedCentroids(this->level_data_indices, this->data, this->k, this->centroids);
        }




        this->clusterMembers = optimiseClusterMembership(this->level_data_indices, this->data, this->centroids, processor_count);

        bool escape = false;
        int iteration = 0;
        // best cluster memebership up here
        while (escape == false)
        {
            auto optimalSelectionResults = optimiseCentroidSelectionAndComputeClusterCost(this->level_data_indices, this->data, centroids, this->clusterMembers, processor_count);
            auto cost = get<0>(optimalSelectionResults);
            auto clusterMembership = get<1>(optimalSelectionResults);
            auto centroids = get<2>(optimalSelectionResults);
            clusterMembership = optimiseClusterMembership((this->level_data_indices), this->data, centroids, processor_count);
            if (cost < this->currentPermutationCost)
            {
                cout << "Cost improving [id | currentCost | oldCost]:[";
                centroidPrinter(this->id);
                cout << " | " << cost << " | " << this->currentPermutationCost << endl;
                cout << endl;
                this->currentPermutationCost = cost;
                this->clusterMembers = clusterMembership;
                this->centroids = centroids;
                // cout << "current centroids"; centroidPrinter(this->centroids); cout << endl;
            }
            else
            {
                // cout << "cost not improving (cost,best):(" << cost << "|" << this->currentPermutationCost << endl;
                escape = true;
            }
            iteration++;
        }


        // build children
        // FIXME SHOULD SKIP THIS IS WE HAVE BEEN DESERIALISED AKA THE CHILDREN CAN ALREADY EXIST
        for (int i = 0; i < (this->centroids.size()); i++)
        {
            // these are local bozo convert to global so each level is global
            auto localClusterMembership = this->clusterMembers[i]; // centroid_id
            vector <int> level_data_indices = {};
            for (int j = 0; j < localClusterMembership.size(); j++) {
                level_data_indices.push_back(this->level_data_indices[localClusterMembership[j]]);
            }
            // fixme convert these into global
            vector<int> newId = this->id;                      // {};
            /*for (int k = 0; k < this->id.size(); k++) backup just in case
            {
                newId.push_back(this->id[k]);
            }*/
            newId.push_back(i);
            auto child = TrainingNode(this->root->vocTreeFile, this->data, level_data_indices, newId, {0}, this->k, this->processor_count, this, this->root);
            this->children.push_back(child);
        }
        this->finished = true;
    }
    void process()
    {
        this->fit_level();
        // children are built by now

        // deal with children
        for (int i = 0; i < this->children.size(); i++)
        {
            this->children[i].process();
            // save on big level progress
            if (this->parent == nullptr)
            {
                this->save();
            }
        }
    }
    void save()
    {
        json treeData = this->root->serialise();
        std::string stringTreeData = treeData.dump();
        // save to file
        std::ofstream file(this->root->vocTreeFile);
        file << stringTreeData;
    }
    // extensions for model building
    bool finished = false;
    json serialise()
    {
        json jnode;
        jnode["id"] = this->id;
        jnode["data_indices"] = this->level_data_indices;
        jnode["centroids"] = this->centroids;
        jnode["currentPermutationCost"] = this->currentPermutationCost;
        jnode["k"] = this->k;
        jnode["concurrency"] = this->processor_count;
        jnode["finished"] = this->finished;
        auto children = json::array();
        for (auto child = this->children.begin(); child != this->children.end(); ++child)
        {
            children.push_back(child->serialise());
        }
        jnode["children"] = children;
        jnode["clusterMembers"] = this->clusterMembers;
        return jnode;
    }
    Node toNode() {}
    TrainingNode(string modelName, std::shared_ptr<FeatureMatrix> data, vector<int> level_data_indices, vector<int> id, vector<int> centroids, int k, int processor_count, TrainingNode *parent, TrainingNode *root) : Node(id, centroids, parent, root)
    {
        this->data = data;
        this->level_data_indices = level_data_indices;
        this->centroids = centroids;
        this->parent = parent;
        this->id = id;
        this->root = root;
        this->k = k;
        if (root == nullptr)
        { // if no root is given assume self
            this->root = this;
        }
        this->root->vocTreeFile = modelName;
        this->processor_count = processor_count;

        if (level_data_indices.size() < k * 2) // fixme what is this value?
        {
            // we are a leaf node
            this->finished = true;
        }
    }
};

TrainingNode deserialise(string modelName, std::shared_ptr<FeatureMatrix> data, json model, TrainingNode *parent, TrainingNode *root)
{
    /*
ns::person p {
    j["name"].get<std::string>(),
    j["address"].get<std::string>(),
    j["age"].get<int>()
};
    */
    cout << "in deserialise" << endl;
    auto level_data_indices = model["data_indices"].get<vector<int>>();
    auto id = model["id"].get<vector<int>>();
    auto centroids = model["centroids"].get<vector<int>>();
    auto currentPermutationCost = model["currentPermutationCost"].get<long long>();
    auto k = model["k"].get<int>();
    auto concurrency = model["concurrency"].get<int>();
    auto children = model["children"];
    auto clusterMembers = model["clusterMembers"].get<map<int, vector<int>>>();

    TrainingNode node = TrainingNode(modelName, data, level_data_indices, id, centroids, k, concurrency, parent, root);

    node.clusterMembers = clusterMembers;
    node.currentPermutationCost = currentPermutationCost;
    node.processor_count = concurrency;

    for (int i = 0; i < children.size(); i++)
    {
        auto childModel = children[i];
        TrainingNode childNode = deserialise(modelName, data, childModel, &node, root);
        node.children.push_back(childNode);
    }

    return node;
}


ComputeNode trainingNodeToComputeNode(TrainingNode *parentTrainingNode, ComputeNode *parentComputeNode)
{

    ComputeNode *rootNode = parentComputeNode->root;

    if (parentTrainingNode->centroids.size() == 0) // centroids.size() == 1
    {
        if (parentTrainingNode->level_data_indices.size() == 0)
        {
            // parent is leaf
            parentComputeNode->isLeaf = true;
        }
        else
        {
            // add leaves
            for (int i = 0; i < parentTrainingNode->level_data_indices.size(); i++)
            {
                vector<int> leafNodeId = parentComputeNode->id;
                leafNodeId.push_back(i);
                //             auto centroidLocalId = parentTrainingNode->centroids[i];
                ComputeNode leaf = ComputeNode();
            }
        }
    }
    else
    {
        for (int i = 0; i < parentTrainingNode->children.size(); i++)
        {
            // get feature id
            auto centroidLocalId = parentTrainingNode->centroids[i];
            auto centroidGlobalId = parentTrainingNode->level_data_indices[centroidLocalId];
            auto featureId = centroidGlobalId;

            // get node id
            auto nodeId = parentComputeNode->id;
            nodeId.push_back(i);

            auto childNode = ComputeNode(nodeId, featureId, parentComputeNode, rootNode);
            parentComputeNode->children.push_back(childNode);

            // process child training node
            auto childTrainingNode = parentTrainingNode->children[i];
            trainingNodeToComputeNode(&childTrainingNode, &childNode);
        }
    }

    /*ComputeNode rootNode;
    if (trainingNode.parent == nullptr)
    {
        rootNode = ComputeNode(); // root
    }

    if (trainingNode.centroids.size() == 0)
    {
        // no children could be a leaf or parent could be a leaf
        if (trainingNode.level_data_indices.size() == 0)
        {
            // parent is leaf
            parent->isLeaf = true;
        }
        else
        {
            // add leaves
            for (int i = 0; i < trainingNode.level_data_indices.size(); i++)
            {
                vector<int> leafNodeId = parent->id;
                leafNodeId.push_back(i);
                ComputeNode leaf = ComputeNode();
            }
        }
    }
    else
    {
        for (int i = 0; i < trainingNode.children.size(); i++)
        {
            // get feature id
            auto centroidLocalId = trainingNode.centroids[i];
            auto centroidGlobalId = trainingNode.level_data_indices[centroidLocalId];
            auto featureId = centroidGlobalId;

            // get node id
            auto nodeId = trainingNode.id;
            nodeId.push_back(i);
        }
    }*/
};

void makeComputeModelFromTrainingModel(TrainingNode *rootTrainingNode) {
    // create root compute node
    ComputeNode root = ComputeNode();
    trainingNodeToComputeNode(rootTrainingNode, &root);
}


// factory method

// pair of leaf paths (aka word ids) and the root node;
pair<vector<vector<int>>, Node> getModel(char *filename)
{
} // what about leaf paths
// can an empty vector be a valid key index? probably yes

/*
"partitions": 8,
     "centroids": [0, 1, 2, 3];
     "membership": {
         0: [{recursive}]
         1: []
         2: []
         3: []
     }
*/

json read_jsonfile(string filename)
{
    std::ifstream i(filename.c_str());
    json j;
    i >> j;
    return j;
}

void trainModel(string modelName)
{
    // say model name is: midsized
    // then feature file is midsized_features.yml
    string featureFile = "data/" + modelName + "_features.yml";
    // then the voc tree is midsized_voctree.json
    string vocTree = "data/" + modelName + "_voctree.json";

    // check files exist
    bool featureFileExists = file_exists(featureFile);
    bool treeFileExists = file_exists(vocTree);

    if (featureFileExists == false)
    {
        cout << "Feature file does not exist" << endl;
        exit(1);
    }

    // readFeaturesFromFile
    cv::Mat dataMat = readFeaturesFromFile((char *)featureFile.c_str());

    vector<int> indices = getRange(dataMat.rows);
    auto data = matToVector(dataMat);
    auto sData = std::make_shared<FeatureMatrix>(data);

    vector<int> origIndicies = getRange(100);

    // if the voctree file exists
    if (treeFileExists == true)
    {
        exit(1); // this is broken fixme
        json model = read_jsonfile(vocTree);
        TrainingNode rootNode = deserialise(vocTree, sData, model, nullptr, nullptr);
        rootNode.process();
        rootNode.save();
    }
    else
    {
        // {47743, 211873, 225696, 300333, 316793, 324287, 460397, 485301
        TrainingNode rootNode = TrainingNode(vocTree, sData, indices, {}, {0}, 8, 10, nullptr, nullptr);
        rootNode.process();
        rootNode.save();
    }

    cout << " All training finished " << endl;
}